Name:           lumimux
Version:        0.1.0
Release:        1%{?dist}
Summary:        Terminal multiplexer with GNU Screen keybindings

License:        MIT-0
URL:            https://github.com/jonesMUX/lumimux
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc, make, musl-tools
Requires:       (none)

%description
lumiMUX is a rewrite of GNU Screen with git-style sub-commands,
tiled pane splits, configurable themes, and a built-in splash screen.
Single static binary with no runtime dependencies.

%prep
%autosetup

%build
make CC=musl-gcc LDFLAGS=-static TARGET_TRIPLET=x86_64-linux-musl RELEASE=1 lumi

%install
install -Dm 755 _out/x86_64-linux-musl/bin/lumi %{buildroot}%{_bindir}/lumi
for cmd in attach mserver new list version kill detach new-window \
           reload send-input send-keys splash; do
    ln -sf lumi %{buildroot}%{_bindir}/lumi-$cmd
done

%check
make CC=musl-gcc LDFLAGS=-static TARGET_TRIPLET=x86_64-linux-musl run-tests

%files
%{_bindir}/lumi
%{_bindir}/lumi-attach
%{_bindir}/lumi-detach
%{_bindir}/lumi-kill
%{_bindir}/lumi-list
%{_bindir}/lumi-mserver
%{_bindir}/lumi-new
%{_bindir}/lumi-new-window
%{_bindir}/lumi-reload
%{_bindir}/lumi-send-input
%{_bindir}/lumi-send-keys
%{_bindir}/lumi-splash
%{_bindir}/lumi-version

%changelog
* Sat Apr 05 2026 Jon Mayo <jon@example.com> - 0.1.0-1
- Initial package
