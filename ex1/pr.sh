#/bin/bash
YES=

if [ "$1" = "-y" ]; then
	shift
	YES=1
fi

STEP () {
	if [ -z "$YES" ]; then
		read -p 'Do it? (Y/n) ' c
		[ -z "$c" ] || [ "$c" = "Y" ] || [ "$c" = "y" ]
	else
		true
	fi
}

set -v
cd ${HOME}

STEP Install git, vim, and other tools && {
	apt-get install git gcc vim python python-dev python-setuptools bash-completion
stress bc htop
}

STEP Enable greek UTF-8 locale && {
	sed -i -e 's/^# *\(el_GR.UTF-8\)$/\1/g' /etc/locale.gen locale-gen
}

STEP Enable greek UTF-8 typing in shell && {
	touch ~/.bashrc
	sed -i -e '/^export LC_CTYPE=/d' ~/.bashrc
	echo 'export LC_CTYPE=el_GR.UTF-8' '>>' ~/.bashrc # Now you have to re-login to
enable it
}

STEP Get cgmon source and start working && {
	mv -f cgmon cgmon-$(date -Isec)
	git clone git@snf-808176.vm.okeanos.grnet.gr:~/cgmon
	cd cgmon
	python setup.py install
	cgmon complete > /etc/bash_completion.d/cgmon
	cd ${HOME}
}

STEP Initialize cgmon hierarchy && {
	cd cgmon
	./prepare.sh
	cd ${HOME}
}

