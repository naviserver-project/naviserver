# WHAT IS THIS?

This is the source distribution of **NaviServer**, a versatile
multiprotocol (HTTP(S), etc.) server written in C/Tcl. It is designed
to be easily extended in either language, allowing you to build
innovative and scalable websites and services.

## Contents

1. [Introduction](#introduction)
2. [Documentation](#documentation)
3. [Compiling and Installing](#compiling-and-installing)
4. [Install NSF/XOTcl](#install-nsfxotcl)
5. [Mailing Lists](#mailing-lists)

---

## 1. Introduction

NaviServer is maintained, enhanced, and distributed freely by the open
source community. Download NaviServer or browse its bug/patch database
on the [SourceForge site](https://sourceforge.net/projects/naviserver).

The source code is hosted on [GitHub](https://github.com/naviserver-project/naviserver) and
additional information can be found on the [Tcl Wiki](https://wiki.tcl-lang.org/page/NaviServer).

NaviServer is released as free and open source software. For full
licensing details, please refer to the `license.terms` file included
in this distribution. 

---

## 2. Documentation


Documentation is located in the `doc` subdirectory. Although it is
still a work in progress, it will eventually be available in both Unix
nroff format (ideal for viewing with the `man` command) and HTML
format (compatible with any modern web browser).

The latest development documentation is available online at:  
<https://naviserver.sourceforge.io/n/toc.html>


---

## 3. Compiling and Installing

NaviServer compiles and runs on various platforms, including FreeBSD,
Linux, Solaris, macOS 10.2+, and Windows.

An install script for Unix platforms (including macOS) with extensive
configuration options is available from:  
<https://github.com/gustafn/install-ns>

The following sections describe, in detail, how to compile and install
NaviServer along with NSF.


### 3a. Download, Configure, Build and Install Tcl 8.5 or Better

If you already have Tcl installed and it was built with threads
enabled, you can use it. Otherwise, download the latest Tcl release
from [tcl-lang.org](https://www.tcl-lang.org/) and follow the included
README instructions. You may install Tcl in the same directory where
you plan to install NaviServer (e.g., `/usr/local/ns`, recommended to
avoid version mismatches) or in a separate location.

> **Note:** NaviServer 4.99.* requires Tcl 8.5 or Tcl 8.6, while NaviServer 5 support Tcl 9.

On a Unix-like system, run:

```bash
gunzip < tcl8.6.13-src.tar.gz | tar xvf -
cd tcl8.6.13/unix
./configure --prefix=/usr/local/ns --enable-threads --enable-symbols
make install
```

### 3b. Install GNU Make

The NaviServer makefiles require GNU Make. Verify your installation
with:

```bash
make -v
```

If necessary, install GNU Make from [gnu.org](https://www.gnu.org/) or
get if via the package manager of your operating system.


### 3c. Download, Configure, Build, and Install NaviServer

**Official releases:**  
<https://sourceforge.net/projects/naviserver/files/>

**Latest development source code (Git repository):**  
<https://github.com/naviserver-project/naviserver/>

> **Git Branches:**  
> - `main` (latest development code)  
> - `release/4.99` (bug fixes for NaviServer 4.99.*)

To compile official releases, execute:

```bash
gunzip < naviserver-5.0.0.tar.gz | tar xvf -
cd naviserver-5.0.0
./configure --prefix=/usr/local/ns --with-tcl=/usr/local/ns/lib --enable-symbols
make
su -c 'make install'
```

**Configure Script Options:**

- `--with-tcl=/usr/local/ns/lib`  
  Locate `tclConfig.sh` in the specified directory.

- `--with-zlib=/usr`  
  Specify the location of the zlib headers (e.g., install via `yum install zlib-devel` on Fedora).

- `--enable-symbols`  
  Build with debug symbols enabled (recommended).

- `--prefix=/usr/local/ns`  
  Set the installation directory for programs, man pages, and runtime
  files. If you compile Tcl, use also for Tcl the same `--prefix` location.

To compile with the Purify tool, run:

```bash
make PURIFY="purify -cache-dir=/home/joe/my-cache-dir" install
```

If you clone NaviServer from GitHub, run `./autogen.sh` with the
configure options (instead of `./configure`) to generate the
makefiles. You will need recent versions of autoconf, automake, and
the `dtplite` package from `tcllib` to build the documentation.

> **Tip:** Use `make build-doc` to generate documentation; otherwise, `make install` may complain.

See `make help` for additional assistance.

### 3d. Create and Edit a NaviServer Configuration File

By convention, NaviServer uses a configuration file named `nsd.tcl`.

```bash
cd /usr/local/ns
cp sample-config.tcl nsd.tcl
vi nsd.tcl
```

Sample files are provided:

- **nsd-config.tcl** Simple configuration file, suitable for simple applications
- **sample-config.tcl** includes every configuration option and its default value (remove unused options).  
- **openacs-config.tcl** NaviServer configuration file for
  [OpenACS](https://openacs.org/)  
  
Find the documentation for configuring NaviServer in
[admin-config](https://naviserver.sourceforge.io/5.0/manual/files/admin-config.html).

### 3e. Run the Server in a Shell

Test NaviServer by running:

```bash
cd /usr/local/ns
./bin/nsd -f -t conf/nsd.tcl
```

The `-f` option runs the server in the foreground with important log messages directed to your terminal.

### 3f. Install Additional Modules

For tarball releases, compatible modules are provided via
SourceForge. For example, to install a module named `nsfoo`:


```bash
gunzip < naviserver-5.0.0-modules.tar.gz | tar xvf -
cd modules/nsfoo
make install NAVISERVER=/usr/local/ns
```

Alternatively, clone modules from GitHub:

```bash
git clone https://github.com/naviserver-project/nsfoo.git
cd nsfoo
make install NAVISERVER=/usr/local/ns
```

For a complete list of modules, visit [GitHub repositories](https://github.com/orgs/naviserver-project/repositories).

### 3g. Compile for Windows with Msys + Mingw

Download the minimal environment from [https://sourceforge.net/projects/mingw/files/](https://sourceforge.net/projects/mingw/files/).

1. Download the minimal Msys + Mingw environment from [SourceForge](https://sourceforge.net/projects/mingw/files/).
2. Extract the zip file and follow the instructions in `README.TXT` to launch the msys shell.
3. In the msys shell, run:

    ```bash
    cd /c/naviserver-5.0.0
    ./configure --prefix=c:/naviserver --with-tcl=c:/naviserver/lib
    make install
    ```

*Note:* This example assumes Tcl is built with Mingw using the prefix `c:/naviserver`.

### 3h. Compile for Windows with MSVC

Update the `tcl_64` and `tcllib_64` variables in `Makefile.win32` (in the NaviServer root directory) and check settings in `include/Makefile.win32` (such as `HAVE_OPENSSL_EVP_H` and `openssl_64`).

Run the appropriate Microsoft build environment script, for example:

- `"C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"`  
- `"%ProgramFiles%\Microsoft SDKs\Windows\v7.1\Bin\SetEnv.Cmd" /Debug /x64 /win7`

Then execute:

```cmd
nmake -f Makefile.win32
```

### 3i. Cross-Compiling for Windows 64-bit (using gcc/mingw)

Some Mingw settings may not be auto-detected. Specify them explicitly:

```bash
./configure --host=x86_64-w64-mingw32 --enable-64-bit \
      --prefix=<path> --with-zlib=<path> --with-openssl=<path> --with-tcl=<path>/lib

CFLAGS="-DHAVE_INET_PTON -DHAVE_INET_NTON -DHAVE_GETADDRINFO -D_WIN32_WINNT=0x600" \
      LDFLAGS="-static-libgcc" \
      make LIBLIBS="-Wl,-Bstatic -lpthread -Wl,-Bdynamic"
```

Since the installation script does not expect the `.exe` extension, copy the executables without the extension as a workaround:

```bash
cp nsthread/nsthreadtest.exe nsthread/nsthreadtest
cp nsd/nsd.exe nsd/nsd
cp nsproxy/nsproxy.exe nsproxy/nsproxy
make install
```

### 3j. (Optional) To Compile for Windows with Cygwin

*(Instructions for compiling with Cygwin will be provided if available.)*

---

## 4. Install NSF/XOTcl

NSF/XOTcl provides essential functions (e.g., cryptographic and
reverse proxy capabilities) that enhance NaviServer’s
features. Although optional for users of Tcl 8.5, these components are
recommended for all installations.

Download NSF/XOTcl from either
[SourceForge](https://sourceforge.net/projects/next-scripting/) or
[GitHub](https://github.com/nm-wu/nsf), and install it into the
NaviServer source tree using the same `--prefix` as NaviServer:

```bash
git clone https://github.com/nm-wu/nsf
cd nsf
./configure --prefix=/usr/local/ns
make
sudo make install
```

---

## 5. Mailing Lists

Join the NaviServer mailing lists to discuss configuration, development, and future directions. Visit:  
<https://sourceforge.net/projects/naviserver>

Thank you for your interest in NaviServer. We hope you find it
valuable and look forward to your contributions on our mailing lists.

---

