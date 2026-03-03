Name:           jarvisagent
Version:        0.1
Release:        1%{?dist}
Summary:        Parallel AI-driven automation with C++ backend and React frontend
License:        GPL-3.0-only
URL:            https://github.com/beaumanvienna/JarvisAgent
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++ make premake nodejs npm
BuildRequires:  python3-devel python3-pip zlib-devel ncurses-devel
Requires:       python3 python3-pip ncurses-libs zlib bash

%global debug_package %{nil}

%description
JarvisAgent is a C++ backend / React frontend application for parallel
AI-driven automation. It dispatches many concurrent AI requests for bulk
processing workloads, supports visual DAG workflow editing, and ships with
an ncurses terminal UI and a browser-based React dashboard.

%prep
# Guard for dry-run: build-rpm.sh pre-populates BUILDROOT and skips
# %prep/%build via rpmbuild --noprep.  When source tarball is available
# this runs normally.
%setup -q -n JarvisAgent

%build
# Guard for dry-run: skip if no source tree
if [ ! -f premake5.lua ]; then
    echo "==> No source tree — skipping %%build (dry-run)"
else
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
# When build-rpm.sh pre-populates BUILDROOT, skip the install phase.
if [ -f %{buildroot}/opt/jarvisagent/bin/jarvisAgent ]; then
    echo "==> BUILDROOT already populated (build-rpm.sh) — skipping %%install"
else
rm -rf %{buildroot}

%define _instdir %{buildroot}/opt/jarvisagent

# Binary
install -Dm755 bin/Release/jarvisAgent %{_instdir}/bin/jarvisAgent

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
for jcwf in aiCarMaintenancePipeline aiZipDemo exampleMakefile4 \
            make-example portfolioDividendAnalysis \
            vehicleTroubleshootingGuide; do
    install -m644 "example/workflows/${jcwf}.jcwf" %{_instdir}/workflows/
done
# Loose input files needed by the example workflows
for f in app.cpp lib1.cpp lib2.cpp main.cpp mylib.h \
         message_engine_question.txt message_tire_question.txt \
         message_unclear_question.txt port62pos.csv; do
    install -m644 "example/workflows/$f" %{_instdir}/workflows/ 2>/dev/null || true
done
# Symlink used by aiCarMaintenancePipeline
ln -sf message_engine_question.txt %{_instdir}/workflows/message.txt

# Example config
install -m644 config.json %{_instdir}/config.json.example

# Runtime directories
install -dm755 %{_instdir}/queue
install -dm755 %{_instdir}/log

# Documentation
install -dm755 %{_instdir}/doc
install -m644 README.md %{_instdir}/doc/README.md
install -m644 doc/JC_Workflow_Specification.md %{_instdir}/doc/JC_Workflow_Specification.md 2>/dev/null || true

# Launcher script
install -Dm755 /dev/stdin %{buildroot}/usr/bin/jarvisagent <<'EOF'
#!/usr/bin/env bash
cd /opt/jarvisagent || { echo "Error: /opt/jarvisagent not found"; exit 1; }
if [[ ! -f config.json ]]; then
    echo "No config.json found in /opt/jarvisagent/"
    echo "Copy the example and edit it:"
    echo "  sudo cp /opt/jarvisagent/config.json.example /opt/jarvisagent/config.json"
    exit 1
fi
exec ./bin/jarvisAgent "$@"
EOF
fi

%post
echo "==> Creating Python virtual environment in /opt/jarvisagent/.venv ..."
python3 -m venv /opt/jarvisagent/.venv

echo "==> Installing Python tools (markitdown, md2pdf-mermaid, playwright) ..."
/opt/jarvisagent/.venv/bin/pip install --quiet "markitdown[all]" md2pdf-mermaid playwright

echo "==> Installing Playwright Chromium ..."
/opt/jarvisagent/.venv/bin/playwright install chromium

echo ""
echo "==> JarvisAgent installed to /opt/jarvisagent/"
echo ""
echo "    To get started:"
echo "      1. sudo cp /opt/jarvisagent/config.json.example /opt/jarvisagent/config.json"
echo "      2. Edit config.json (set API keys, queue/workflow paths)"
echo "      3. Run: jarvisagent"
echo ""

%postun
if [ "$1" = 0 ]; then
    echo "==> Removing Python virtual environment ..."
    rm -rf /opt/jarvisagent/.venv
    echo "==> You may want to remove /opt/jarvisagent/ manually if custom data remains."
fi

%files
/opt/jarvisagent/
/usr/bin/jarvisagent

%changelog
* Sun Mar 01 2026 JC Technolabs <https://github.com/beaumanvienna> - 0.1-1
- Initial RPM package
