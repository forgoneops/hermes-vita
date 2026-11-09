#!/bin/sh
# Put the hermes-vita plugin where Hermes looks for plugins. It stays OFF until you enable it yourself:
#   hermes plugins enable hermes-vita
set -e
cd "$(dirname "$0")/.."
mkdir -p "$HOME/.hermes/plugins" "$HOME/.hermes/skills/hermes-vita"
rm -rf "$HOME/.hermes/plugins/hermes-vita"; cp -r hermes-plugin/hermes-vita "$HOME/.hermes/plugins/hermes-vita"
cp skills/hermes-vita/SKILL.md "$HOME/.hermes/skills/hermes-vita/SKILL.md"
echo "plugin and skill copied. Enable the plugin:  hermes plugins enable hermes-vita   (then restart Hermes)"
