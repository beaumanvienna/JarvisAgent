class Jarvisagent < Formula
  desc "Parallel AI-driven automation with C++ backend and React frontend"
  homepage "https://github.com/beaumanvienna/JarvisAgent"
  url "https://github.com/beaumanvienna/JarvisAgent.git", branch: "main", using: :git
  version "0.1"
  license "GPL-3.0-only"

  depends_on "premake" => :build
  depends_on "node" => :build
  depends_on "python@3"

  def install
    # Generate Makefiles
    system "premake5", "gmake"

    # Build C++ release binary
    ENV["MAKEFLAGS"] = "-j#{ENV.make_jobs}"
    system "make", "config=release"

    # Build React dashboard
    cd "dashboard/ui" do
      system "npm", "install"
      system "npm", "run", "build"
    end

    # Build React workflow editor
    cd "workflow-editor/ui" do
      system "npm", "install"
      system "npm", "run", "build"
    end

    # Install to prefix
    prefix.install "bin/Release/jarvisAgent" => "bin/jarvisAgent"

    # React UIs
    (prefix/"dashboard/ui").install "dashboard/ui/dist"
    (prefix/"workflow-editor/ui").install "workflow-editor/ui/dist"

    # Scripts (excluding __pycache__)
    prefix.install "scripts"
    rm_rf Dir[prefix/"scripts/**/__pycache__"]

    # Example workflows (curated list — no subdirs, no build artifacts)
    (prefix/"workflows").mkpath
    %w[
      aiCarMaintenancePipeline aiZipDemo exampleMakefile4
      make-example portfolioDividendAnalysis
      vehicleTroubleshootingGuide
    ].each { |w| (prefix/"workflows").install "example/workflows/#{w}.jcwf" }
    # Loose input files needed by the example workflows
    %w[
      app.cpp lib1.cpp lib2.cpp main.cpp mylib.h
      message_engine_question.txt message_tire_question.txt
      message_unclear_question.txt port62pos.csv
    ].each { |f| (prefix/"workflows").install "example/workflows/#{f}" if File.exist?("example/workflows/#{f}") }
    # Symlink used by aiCarMaintenancePipeline
    ln_sf "message_engine_question.txt", prefix/"workflows/message.txt"

    # Example config
    prefix.install "config.json" => "config.json.example"

    # Runtime directories
    (prefix/"queue").mkpath
    (prefix/"log").mkpath

    # Documentation
    doc.install "README.md"
    doc.install "doc/JC_Workflow_Specification.md" if File.exist?("doc/JC_Workflow_Specification.md")

    # Launcher script
    (bin/"jarvisagent").write <<~EOS
      #!/bin/bash
      JADIR="#{prefix}"
      cd "$JADIR" || { echo "Error: $JADIR not found"; exit 1; }
      case "${1:-}" in
          --help|-h|--version|-v) exec "#{prefix}/bin/jarvisAgent" "$@" ;;
      esac
      if [[ ! -f config.json ]]; then
          echo "No config.json found in $JADIR/"
          echo "Copy the example and edit it:"
          echo "  cp #{prefix}/config.json.example #{prefix}/config.json"
          exit 1
      fi
      exec "#{prefix}/bin/jarvisAgent" "$@"
    EOS
  end

  def post_install
    # Create Python venv
    venv = prefix/".venv"
    unless venv.exist?
      system "python3", "-m", "venv", venv.to_s
      system venv/"bin/pip", "install", "--quiet", "--upgrade", "pip"
      system venv/"bin/pip", "install", "--quiet", "markitdown[all]", "md2pdf-mermaid", "playwright"
      system venv/"bin/playwright", "install", "chromium"
    end
  end

  def caveats
    <<~EOS
      JarvisAgent is installed at:
        #{prefix}

      To get started:
        1. cp #{prefix}/config.json.example #{prefix}/config.json
        2. Edit config.json (set API keys, queue/workflow paths)
        3. Activate the Python venv: source #{prefix}/.venv/bin/activate
        4. Run: jarvisagent

      The Python venv with markitdown/md2pdf/playwright is at:
        #{prefix}/.venv
    EOS
  end

  test do
    assert_match "jarvisAgent", shell_output("#{bin}/jarvisagent --help 2>&1", 1)
  end
end
