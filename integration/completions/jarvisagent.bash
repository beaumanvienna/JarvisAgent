# Bash completion for jarvisagent.sh
# Source this file or add to ~/.bashrc:
#   source /path/to/jarvisAgent/completions/jarvisagent.bash

_jarvisagent_completions()
{
    local cur="${COMP_WORDS[COMP_CWORD]}"
    local opts="--studio --engine --help"
    COMPREPLY=($(compgen -W "$opts" -- "$cur"))
}

complete -F _jarvisagent_completions jarvisagent.sh
complete -F _jarvisagent_completions ./jarvisagent.sh
