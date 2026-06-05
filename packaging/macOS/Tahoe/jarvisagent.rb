class Jarvisagent < Formula
  desc "Parallel AI-driven automation with C++ backend and React frontend"
  homepage "https://github.com/beaumanvienna/JarvisAgent"
  url "https://github.com/beaumanvienna/JarvisAgent.git", branch: "main", using: :git
  version "0.1"
  license "GPL-3.0-only"

  depends_on "premake" => :build
  depends_on "node" => :build
  depends_on "libpq" => :build
  depends_on "python@3"
  depends_on "libpq"
  depends_on "pandoc"

  def install
    # Generate Makefiles
    system "premake5", "gmake"

    # Build C++ release binary
    ENV["MAKEFLAGS"] = "-j#{ENV.make_jobs}"
    system "make", "config=release"

    # Build React dashboard
    cd "code/frontend/dashboard/ui" do
      system "npm", "install"
      system "npm", "run", "build"
    end

    # Build React workflow editor
    cd "code/frontend/workflow-editor/ui" do
      system "npm", "install"
      system "npm", "run", "build"
    end

    # Install to prefix
    prefix.install "bin/Release/jarvisAgent" => "bin/jarvisAgent"

    # React UIs
    (prefix/"dashboard/ui").install "code/frontend/dashboard/ui/dist"
    (prefix/"workflow-editor/ui").install "code/frontend/workflow-editor/ui/dist"

    # Scripts (excluding __pycache__)
    prefix.install "scripts"
    rm_rf Dir[prefix/"scripts/**/__pycache__"]

    # Example workflows (curated list — no subdirs, no build artifacts)
    (prefix/"workflows").mkpath
    %w[
      aiCarMaintenancePipeline aiZipDemo
      make-example portfolioDividendAnalysis
    ].each { |w| (prefix/"workflows").install "example/workflows/#{w}.jcwf" }
    # (Input files are bundled inside each .jcwf zip — no loose files to install)

    # Example config
    prefix.install "packaging/config.json.example" => "config.json.example"

    # Runtime directories
    (prefix/"queue").mkpath
    (prefix/"log").mkpath

    # Documentation
    doc.install "README.md"
    doc.install "doc/JC_Workflow_Specification.md" if File.exist?("doc/JC_Workflow_Specification.md")

    # Man page
    man1.install "doc/jarvisagent.1"

    # Launcher script (user-space: creates ~/JarvisAgent with symlinks)
    (bin/"jarvisagent").write <<~EOS
      #!/bin/bash
      set -euo pipefail

      INSTALL_DIR="#{prefix}"
      OPEN_BROWSER=true
      USER_HOME=""

      PASSTHROUGH_ARGS=()
      while [[ $# -gt 0 ]]; do
          case "$1" in
              --home)
                  [[ -z "${2:-}" ]] && echo "Error: --home requires a directory argument" && exit 1
                  USER_HOME="$2"; shift 2 ;;
              --no-browser) OPEN_BROWSER=false; shift ;;
              --help|-h|--version|-v) exec "$INSTALL_DIR/bin/jarvisAgent" "$1" ;;
              *) PASSTHROUGH_ARGS+=("$1"); shift ;;
          esac
      done

      [[ -z "$USER_HOME" ]] && USER_HOME="${JARVISAGENT_HOME:-$HOME/JarvisAgent}"

      [[ ! -d "$USER_HOME" ]] && echo "==> First run: creating working directory at $USER_HOME" && mkdir -p "$USER_HOME"

      # NOTE: scripts/ is deliberately NOT symlinked — see the copy step below.
      for asset in dashboard workflow-editor doc; do
          if [[ -L "$USER_HOME/$asset" ]] || [[ ! -e "$USER_HOME/$asset" ]]; then
              ln -sfn "$INSTALL_DIR/$asset" "$USER_HOME/$asset"
          fi
      done

      mkdir -p "$USER_HOME/bin"
      if [[ -L "$USER_HOME/bin/jarvisAgent" ]] || [[ ! -e "$USER_HOME/bin/jarvisAgent" ]]; then
          ln -sfn "$INSTALL_DIR/bin/jarvisAgent" "$USER_HOME/bin/jarvisAgent"
      fi

      mkdir -p "$USER_HOME/queue" "$USER_HOME/log" "$USER_HOME/workflows"

      if [[ -d "$INSTALL_DIR/workflows" ]] && [[ -z "$(ls -A "$USER_HOME/workflows" 2>/dev/null)" ]]; then
          cp -a "$INSTALL_DIR/workflows/"* "$USER_HOME/workflows/" 2>/dev/null || true
          echo "==> Copied example workflows to $USER_HOME/workflows/"
      fi

      # Copy bundled scripts as REAL files (not a symlink). Workflow tasks reference
      # scripts/ via path-confined relative paths; confinement follows a symlink out
      # of the working dir and rejects it, breaking every script task.
      [[ -L "$USER_HOME/scripts" ]] && rm -f "$USER_HOME/scripts"
      if [[ -d "$INSTALL_DIR/scripts" ]] && [[ ! -d "$USER_HOME/scripts" ]]; then
          mkdir -p "$USER_HOME/scripts"
          cp -a "$INSTALL_DIR/scripts/." "$USER_HOME/scripts/" 2>/dev/null || true
          echo "==> Copied bundled scripts to $USER_HOME/scripts/"
      fi

      if [[ ! -f "$USER_HOME/config.json" ]] && [[ -f "$INSTALL_DIR/config.json.example" ]]; then
          cp "$INSTALL_DIR/config.json.example" "$USER_HOME/config.json"
          echo "==> Created $USER_HOME/config.json from example"
          echo "    Please edit it to set your API keys."
      fi

      # Note: the server mints its own self-signed TLS cert on first start when
      # certs/j9t-{cert,key}.pem are absent (see WebServer::Start) — no
      # launcher-side openssl provisioning is needed.

      if [[ ! -d "$USER_HOME/.venv" ]]; then
          echo "==> Creating Python virtual environment at $USER_HOME/.venv ..."
          python3 -m venv "$USER_HOME/.venv" 2>/dev/null || true
          if [[ -d "$USER_HOME/.venv" ]]; then
              "$USER_HOME/.venv/bin/pip" install --quiet --upgrade pip 2>/dev/null || true
              echo "==> Installing Python tools (markitdown) ..."
              "$USER_HOME/.venv/bin/pip" install --quiet "markitdown[all]" 2>/dev/null || true
          fi
      fi

      [[ -f "$USER_HOME/.venv/bin/activate" ]] && source "$USER_HOME/.venv/bin/activate"

      if [[ "$OPEN_BROWSER" == true ]]; then
          (sleep 2 && open "https://localhost:8443" 2>/dev/null) &
      fi

      echo "==> Starting JarvisAgent in $USER_HOME"
      echo "    Dashboard: https://localhost:8443"
      echo "    Editor:    https://localhost:8443/editor"
      echo ""
      cd "$USER_HOME"
      exec "$USER_HOME/bin/jarvisAgent" "${PASSTHROUGH_ARGS[@]}"
    EOS
  end

  def caveats
    <<~EOS
      To get started, run:  jarvisagent

      On first launch, the launcher will:
        - Create ~/JarvisAgent with your working data
        - Set up a Python virtual environment
        - Copy example config and workflows
        - Open the dashboard in your browser

      Custom working directory:  jarvisagent --home /path/to/dir
      Skip browser:              jarvisagent --no-browser

      PDF workflow (vehicleTroubleshootingGuide) also requires mmdc:
        npm install -g @mermaid-js/mermaid-cli@10.x
    EOS
  end

  test do
    assert_match "jarvisAgent", shell_output("#{bin}/jarvisagent --help 2>&1", 1)
  end
end
