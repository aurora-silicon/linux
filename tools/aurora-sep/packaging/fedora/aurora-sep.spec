# SPDX-License-Identifier: GPL-2.0-only
#
# Built from tools/aurora-sep: rpmbuild --define "_sourcedir <that directory>"
# (build.sh does this).

Name:           aurora-sep
Version:        1
Release:        3%{?dist}
Summary:        Loader and desktop integration for the Apple SEP fingerprint driver

License:        GPL-2.0-only
URL:            https://github.com/aurora-silicon/linux
Source0:        load-driver
Source1:        aurora-sep.service
Source2:        fprintd-aurora.conf
Source3:        README.md
Source4:        XART-SAFETY.md
Source5:        aurora-sep-ready
Source6:        aurora-sep-ready.service
Source7:        fprintd-no-timeout.conf.in
Source8:        logind-aurora-sep.conf
Source9:        aurora-sep-greeter-fingerprint
Source10:       aurora-sep-greeter-fingerprint.service

BuildArch:      noarch
BuildRequires:  systemd-rpm-macros
# Only to check where fprintd's unit runs it from.
BuildRequires:  fprintd
Requires:       coreutils
Requires:       fprintd
Requires:       keyutils
Requires:       kmod
# Only for aurora-sep-greeter-fingerprint.service, which is off by default.
Suggests:       /usr/bin/gresource
%{?systemd_requires}

%description
Loads the Apple SEP driver (apple_sep) with xART writes enabled, waits until
the secure enclave seals and unseals a trusted key and /dev/sep-bio appears,
and lets fprintd reach /dev/sep-bio. The loader runs at boot, and fprintd
also pulls it in when it starts. Touch ID is then activated, before fprintd
starts, and pressing the Touch ID button suspends rather than powers off.
libfprint needs the aurora driver, which this package does not provide; it is
packaged in https://github.com/aurora-silicon/aurora-sep-userspace. Read XART-SAFETY.md before installing it on
a machine that also runs macOS.

%prep
%setup -q -c -T
cp -p %{SOURCE3} %{SOURCE4} .

%build

%install
# A unit and the helper it runs ($1, installed as $3), pointed at the
# packaged helper.
unit() {
    install -Dpm 0755 "$1" %{buildroot}%{_libexecdir}/"$3"
    install -Dpm 0644 "$2" %{buildroot}%{_unitdir}/"${2##*/}"
    sed -i "s|^\(Exec[A-Za-z]*=\)/usr/local/sbin/$3|\1%{_libexecdir}/$3|" \
        %{buildroot}%{_unitdir}/"${2##*/}"
    if grep -q /usr/local %{buildroot}%{_unitdir}/"${2##*/}"; then exit 1; fi
}
unit %{SOURCE0} %{SOURCE1} aurora-sep-load
unit %{SOURCE5} %{SOURCE6} aurora-sep-ready
unit %{SOURCE9} %{SOURCE10} aurora-sep-greeter-fingerprint
install -Dpm 0644 %{SOURCE2} \
    %{buildroot}%{_unitdir}/fprintd.service.d/aurora-sep.conf
grep -qx 'ExecStart=%{_libexecdir}/fprintd' %{_unitdir}/fprintd.service
sed 's|@FPRINTD@|%{_libexecdir}/fprintd|' %{SOURCE7} \
    >%{buildroot}%{_unitdir}/fprintd.service.d/50-aurora-sep-no-timeout.conf
install -Dpm 0644 %{SOURCE8} \
    %{buildroot}%{_prefix}/lib/systemd/logind.conf.d/50-aurora-sep.conf
# Load the driver at boot, so the enclave is up before the login screen asks.
install -d %{buildroot}%{_presetdir}
echo 'enable aurora-sep.service' >%{buildroot}%{_presetdir}/80-aurora-sep.preset

%post
%systemd_post aurora-sep.service aurora-sep-ready.service aurora-sep-greeter-fingerprint.service

%preun
%systemd_preun aurora-sep.service aurora-sep-ready.service aurora-sep-greeter-fingerprint.service

%postun
# Never restart them: the driver attaches to the enclave once per boot.
%systemd_postun aurora-sep.service aurora-sep-ready.service aurora-sep-greeter-fingerprint.service

%files
%doc README.md XART-SAFETY.md
%{_libexecdir}/aurora-sep-load
%{_libexecdir}/aurora-sep-ready
%{_libexecdir}/aurora-sep-greeter-fingerprint
%{_unitdir}/aurora-sep.service
%{_unitdir}/aurora-sep-ready.service
%{_unitdir}/aurora-sep-greeter-fingerprint.service
%dir %{_unitdir}/fprintd.service.d
%{_unitdir}/fprintd.service.d/aurora-sep.conf
%{_unitdir}/fprintd.service.d/50-aurora-sep-no-timeout.conf
%{_prefix}/lib/systemd/logind.conf.d/50-aurora-sep.conf
%{_presetdir}/80-aurora-sep.preset

%changelog
* Sun Oct 11 2026 Ryan Murray <ryan@aurorasilicon.org> - 1-3
- libfprint with the aurora driver is now built in aurora-sep-userspace

* Sun Oct 11 2026 Ryan Murray <ryan@aurorasilicon.org> - 1-2
- Activate Touch ID at boot, before fprintd starts
- Keep fprintd running
- Suspend rather than power off on the Touch ID button
- Offer fingerprint at the GNOME 51 login screen, off by default

* Sun Oct 11 2026 Ryan Murray <ryan@aurorasilicon.org> - 1-1
- Package the loader, its service and the fprintd policy
