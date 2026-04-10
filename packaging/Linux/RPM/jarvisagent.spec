Name:           jarvisagent
Version:        0.1
Release:        1%{?dist}
Summary:        Parallel AI-driven automation with C++ backend and React frontend
License:        GPL-3.0-only
URL:            https://github.com/beaumanvienna/JarvisAgent
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++ make premake nodejs npm
BuildRequires:  python3-devel python3-pip zlib-devel libpq-devel
Requires:       python3 python3-pip zlib libpq bash
Suggests:       pandoc texlive-latex nodejs npm

%global debug_package %{nil}

%description
JarvisAgent is a C++ backend / React frontend application for parallel
AI-driven automation. It dispatches many concurrent AI requests for bulk
processing workloads, supports visual DAG workflow editing, and ships with
an ncurses terminal UI and a browser-based React dashboard.

%prep
# build-rpm.sh populates BUILD/JarvisAgent and skips %prep via --noprep.
# When building from a source tarball, this runs normally.
%setup -q -n JarvisAgent

%build
# build-rpm.sh pre-builds and uses --noprep, so premake5.lua may not be here.
# When building from a source tarball, it will be present and %build runs normally.
if [ ! -f premake5.lua ]; then
    echo "==> Pre-built tree detected — skipping %%build (build-rpm.sh already compiled)"
else
premake5 gmake --engine
make -j%{?_smp_mflags} config=release

premake5 gmake
make -j%{?_smp_mflags} config=release

cd dashboard/ui
npm install
npm run build
cd ../..

cd workflow-editor/ui
npm install
npm run build
cd ../..
fi

%install
rm -rf %{buildroot}

%define _instdir %{buildroot}/opt/jarvisagent

# Binaries (Studio + Engine)
install -Dm755 bin/Release/jarvisAgent-studio %{_instdir}/bin/jarvisAgent-studio
install -Dm755 bin/Release/jarvisAgent-engine %{_instdir}/bin/jarvisAgent-engine

# React UIs
install -dm755 %{_instdir}/dashboard/ui
cp -r dashboard/ui/dist %{_instdir}/dashboard/ui/dist

install -dm755 %{_instdir}/workflow-editor/ui
cp -r workflow-editor/ui/dist %{_instdir}/workflow-editor/ui/dist

# Scripts (excluding __pycache__)
cp -r scripts %{_instdir}/scripts
find %{_instdir}/scripts -type d -name __pycache__ -exec rm -rf {} + 2>/dev/null || true
chmod +x %{_instdir}/scripts/*.sh

# Example workflows (curated list — no subdirs, no build artifacts)
install -dm755 %{_instdir}/workflows
for jcwf in aiCarMaintenancePipeline aiZipDemo \
            make-example portfolioDividendAnalysis \
            vehicleTroubleshootingGuide; do
    install -m644 "example/workflows/${jcwf}.jcwf" %{_instdir}/workflows/
done
# (Input files are bundled inside each .jcwf zip — no loose files to install)

# Example config
install -m644 packaging/config.json.example %{_instdir}/config.json.example

# Runtime directories
install -dm755 %{_instdir}/queue
install -dm755 %{_instdir}/log

# Documentation
install -dm755 %{_instdir}/doc
install -m644 README.md %{_instdir}/doc/README.md
install -m644 doc/JC_Workflow_Specification.md %{_instdir}/doc/JC_Workflow_Specification.md 2>/dev/null || true

# Man page
install -Dm644 doc/jarvisagent.1 %{buildroot}/usr/share/man/man1/jarvisagent.1
gzip -9 %{buildroot}/usr/share/man/man1/jarvisagent.1

# Launcher scripts (shared across deb/rpm/arch — detects edition from script name)
install -Dm755 jarvisagent-launcher.sh %{buildroot}/usr/bin/jarvisagent
install -Dm755 jarvisagent-launcher.sh %{buildroot}/usr/bin/jarvisagent-engine
ln -sf jarvisagent %{buildroot}/usr/bin/jarvisagent-studio
ln -sf jarvisagent %{buildroot}/usr/bin/j9t

%post
echo ""
echo "==> JarvisAgent installed to /opt/jarvisagent/"
echo ""
echo "    To get started, run:  jarvisagent"
echo ""
echo "    On first launch, the launcher will:"
echo "      - Create ~/JarvisAgent with your working data"
echo "      - Set up a Python virtual environment"
echo "      - Copy example config and workflows"
echo "      - Open the dashboard in your browser"
echo ""
echo "    Custom working directory:  jarvisagent --home /path/to/dir"
echo "    Skip browser:              jarvisagent --no-browser"
echo ""

%postun
if [ "$1" = 0 ]; then
    echo "==> JarvisAgent uninstalled from /opt/jarvisagent/"
    echo "    User data in ~/JarvisAgent (or custom --home) was NOT removed."
    echo "    To clean up user data: rm -rf ~/JarvisAgent"
fi

%files
/opt/jarvisagent/
/usr/bin/jarvisagent
/usr/bin/jarvisagent-engine
/usr/bin/jarvisagent-studio
/usr/bin/j9t
/usr/share/man/man1/jarvisagent.1.gz

%changelog
* Sun Mar 01 2026 JC Technolabs <https://github.com/beaumanvienna> - 0.1-1
- Initial RPM package
