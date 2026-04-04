#compdef jarvisagent.sh

# Zsh completion for jarvisagent.sh
# Add the completions directory to fpath and run compinit, or source directly:
#   fpath=(/path/to/jarvisAgent/completions $fpath)
#   compinit

_jarvisagent() {
    _arguments \
        '--studio[Launch Studio edition (default — full developer IDE)]' \
        '--engine[Launch Engine edition (lean production server)]' \
        '--help[Show usage information]'
}

_jarvisagent "$@"
