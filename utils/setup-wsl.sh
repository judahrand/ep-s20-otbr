# general configuration

echo "$USER ALL=(ALL:ALL) NOPASSWD: ALL" | sudo tee /etc/sudoers.d/$USER
sudo usermod -a -G dialout $USER

cat << EOF > /etc/wsl.conf
[boot]
systemd=true
[interop]
appendWindowsPath=false
EOF

echo alias la='ls -la' >> .bash_aliases
echo alias cd..='cd ..' >> .bash_aliases
echo alias k='kubectl' >> .bash_aliases
echo alias get_idf='. $HOME/esp/esp-idf/export.sh' >> .bash_aliases

sudo apt update
sudo apt upgrade
sudo apt-get install -y apt-transport-https ca-certificates curl gnupg wget unzip

# build env

sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv \
	cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0 jq

# applications

curl -fsSL https://gh.io/copilot-install | bash

(type -p wget >/dev/null || (sudo apt update && sudo apt install wget -y)) \
	&& sudo mkdir -p -m 755 /etc/apt/keyrings \
	&& out=$(mktemp) && wget -nv -O$out https://cli.github.com/packages/githubcli-archive-keyring.gpg \
	&& cat $out | sudo tee /etc/apt/keyrings/githubcli-archive-keyring.gpg > /dev/null \
	&& sudo chmod go+r /etc/apt/keyrings/githubcli-archive-keyring.gpg \
	&& sudo mkdir -p -m 755 /etc/apt/sources.list.d \
	&& echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" | sudo tee /etc/apt/sources.list.d/github-cli.list > /dev/null \
	&& sudo apt update \
	&& sudo apt install gh -y

# git

git config --global pull.rebase true
git config --global rebase.autoStash true
git config --global alias.clog 'log --pretty=format:"%h %s"'
git config --global alias.co checkout
git config --global alias.br branch
git config --global alias.ct commit
git config --global alias.st status
