Name:           rux
Version:        0.3.0
Release:        1%{?dist}
Summary:        Rux: A fast, compiled, strongly typed, multi-paradigm, general-purpose programming language.

License:        MIT
URL:            https://rux-lang.dev/

Source0:        https://github.com/rux-lang/Rux/archive/refs/tags/v%{version}.tar.gz

BuildRequires:  cmake
BuildRequires:  gcc-c++

%description
Rux is a fast, compiled, strongly typed, multi-paradigm, general-purpose programming language.

%prep

%autosetup -n Rux-%{version}

%build

%cmake
%cmake_build

%install

%cmake_install

%files

%{_bindir}/rux

%changelog
* Wed Jun 03 2026 Rux Contributors <https://github.com/rux-lang/Rux> - 0.3.0-1
- Initial release for Copr
