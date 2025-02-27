// Copyright (c) Microsoft Corporation.
// Licensed under the MIT License.

#include "pch.h"
#include "WorkflowBase.h"
#include "DependenciesFlow.h"
#include "InstallFlow.h"
#include "UpdateFlow.h"
#include "ManifestComparator.h"

using namespace AppInstaller::Repository;

namespace AppInstaller::CLI::Workflow
{
    namespace
    {
        bool IsUpdateVersionApplicable(const Utility::Version& installedVersion, const Utility::Version& updateVersion)
        {
            return (installedVersion < updateVersion || updateVersion.IsLatest());
        }

        void AddToPackagesToInstallIfNotPresent(std::vector<Execution::PackageToInstall>& packagesToInstall, Execution::PackageToInstall&& package)
        {
            for (auto const& existing : packagesToInstall)
            {
                if (existing.Manifest.Id == package.Manifest.Id &&
                    existing.Manifest.Version == package.Manifest.Version &&
                    existing.PackageVersion->GetProperty(PackageVersionProperty::SourceIdentifier) == package.PackageVersion->GetProperty(PackageVersionProperty::SourceIdentifier))
                {
                    return;
                }
            }

            packagesToInstall.emplace_back(std::move(package));
        }
    }

    void SelectLatestApplicableUpdate::operator()(Execution::Context& context) const
    {
        auto package = context.Get<Execution::Data::Package>();
        auto installedPackage = context.Get<Execution::Data::InstalledPackageVersion>();
        Utility::Version installedVersion = Utility::Version(installedPackage->GetProperty(PackageVersionProperty::Version));
        ManifestComparator manifestComparator(context, installedPackage->GetMetadata());
        bool updateFound = false;
        bool installedTypeInapplicable = false;

        // The version keys should have already been sorted by version
        const auto& versionKeys = package->GetAvailableVersionKeys();
        for (const auto& key : versionKeys)
        {
            // Check Update Version
            if (IsUpdateVersionApplicable(installedVersion, Utility::Version(key.Version)))
            {
                auto packageVersion = package->GetAvailableVersion(key);
                auto manifest = packageVersion->GetManifest();

                // Check applicable Installer
                auto [installer, inapplicabilities] = manifestComparator.GetPreferredInstaller(manifest);
                if (!installer.has_value())
                {
                    // If there is at least one installer whose only reason is InstalledType.
                    auto onlyInstalledType = std::find(inapplicabilities.begin(), inapplicabilities.end(), InapplicabilityFlags::InstalledType);
                    if (onlyInstalledType != inapplicabilities.end())
                    {
                        installedTypeInapplicable = true;
                    }

                    continue;
                }

                // Since we already did installer selection, just populate the context Data
                manifest.ApplyLocale(installer->Locale);
                context.Add<Execution::Data::Manifest>(std::move(manifest));
                context.Add<Execution::Data::PackageVersion>(std::move(packageVersion));
                context.Add<Execution::Data::Installer>(std::move(installer));

                updateFound = true;
                break;
            }
            else
            {
                // Any following versions are not applicable
                break;
            }
        }

        if (!updateFound)
        {
            if (m_reportUpdateNotFound)
            {
                if (installedTypeInapplicable)
                {
                    context.Reporter.Info() << Resource::String::UpgradeDifferentInstallTechnologyInNewerVersions << std::endl;
                }
                else
                {
                    context.Reporter.Info() << Resource::String::UpdateNotApplicable << std::endl;
                }
            }

            AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_UPDATE_NOT_APPLICABLE);
        }
    }

    void EnsureUpdateVersionApplicable(Execution::Context& context)
    {
        auto installedPackage = context.Get<Execution::Data::InstalledPackageVersion>();
        Utility::Version installedVersion = Utility::Version(installedPackage->GetProperty(PackageVersionProperty::Version));
        Utility::Version updateVersion(context.Get<Execution::Data::Manifest>().Version);

        if (!IsUpdateVersionApplicable(installedVersion, updateVersion))
        {
            context.Reporter.Info() << Resource::String::UpdateNotApplicable << std::endl;
            AICLI_TERMINATE_CONTEXT(APPINSTALLER_CLI_ERROR_UPDATE_NOT_APPLICABLE);
        }
    }

    void UpdateAllApplicable(Execution::Context& context)
    {
        const auto& matches = context.Get<Execution::Data::SearchResult>().Matches;
        std::vector<Execution::PackageToInstall> packagesToInstall;
        bool updateAllFoundUpdate = false;

        for (const auto& match : matches)
        {
            Logging::SubExecutionTelemetryScope subExecution;

            // We want to do best effort to update all applicable updates regardless on previous update failure
            auto updateContextPtr = context.Clone();
            Execution::Context& updateContext = *updateContextPtr;

            updateContext.Add<Execution::Data::Package>(match.Package);

            updateContext <<
                Workflow::GetInstalledPackageVersion <<
                Workflow::ReportExecutionStage(ExecutionStage::Discovery) <<
                SelectLatestApplicableUpdate(false);

            if (updateContext.GetTerminationHR() == APPINSTALLER_CLI_ERROR_UPDATE_NOT_APPLICABLE)
            {
                continue;
            }

            updateAllFoundUpdate = true;

            Execution::PackageToInstall package{
                std::move(updateContext.Get<Execution::Data::PackageVersion>()),
                std::move(updateContext.Get<Execution::Data::InstalledPackageVersion>()),
                std::move(updateContext.Get<Execution::Data::Manifest>()),
                std::move(updateContext.Get<Execution::Data::Installer>().value()) };
            package.PackageSubExecutionId = subExecution.GetCurrentSubExecutionId();

            AddToPackagesToInstallIfNotPresent(packagesToInstall, std::move(package));
        }

        if (!updateAllFoundUpdate)
        {
            context.Reporter.Info() << Resource::String::UpdateNotApplicable << std::endl;
            return;
        }

        context.Add<Execution::Data::PackagesToInstall>(std::move(packagesToInstall));
        context <<
            InstallMultiplePackages(
                Resource::String::InstallAndUpgradeCommandsReportDependencies,
                APPINSTALLER_CLI_ERROR_UPDATE_ALL_HAS_FAILURE,
                { APPINSTALLER_CLI_ERROR_UPDATE_NOT_APPLICABLE });
    }
}