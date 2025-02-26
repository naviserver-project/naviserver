# WHAT IS THIS?

This is the source distribution of NaviServer, a versatile multiprotocol (HTTP(S) et al) server written in C/Tcl. It can be easily extended in either language to create interesting websites and services.

## Contents

1. [Introduction](#introduction)
2. [Documentation](#documentation)
3. [Compiling and Installing](#compiling-and-installing)
4. [Install NSF/XOTcl](#install-nsfxotcl)
5. [Mailing Lists](#mailing-lists)

---

## 1. Introduction

NaviServer is maintained, enhanced, and distributed freely by the open source community. The home for NaviServer downloads and the bug/patch database is located on the [SourceForge site](https://sourceforge.net/projects/naviserver).

Source code is available from the [GitHub site](https://github.com/orgs/naviserver-project/naviserver).

Another resource is the [Tcl wiki](https://wiki.tcl-lang.org/page/NaviServer).

NaviServer is a freely available open source package. See the file `license.terms` for complete information.

---

## 2. Documentation

Documentation is available in the `doc` subdirectory. At this point it is incomplete and is considered a work in progress. Once complete, it will be distributed in both Unix nroff format (suitable for viewing with the Unix `man` command) and HTML format (suitable for viewing with any HTML-compatible browser).

The latest development version is available online:  
<https://naviserver.sourceforge.io/n/toc.html>

---

## 3. Compiling and Installing

NaviServer is known to compile and run on FreeBSD, Linux, Solaris, macOS 10.2+ and Windows.

An install script for Unix platforms (including macOS) with many configuration options is available from:  
<https://github.com/gustafn/install-ns>

The following sections provide detailed instructions on how to compile and install NaviServer and NSF.

### 3a. Download, Configure, Build and Install Tcl 8.5 or Better

You may use the version of Tcl already installed on your machine if it was built with threads enabled (the configure step below will complain if not).

Download the latest Tcl release from [https://www.tcl-lang.org/](https://www.tcl-lang.org/) and follow the instructions in the included README. You may install Tcl within the directory where you intend to install the server (e.g., `/usr/local/ns`) or in another location.

> **Note:** NaviServer 4.99.* requires Tcl 8.5 or Tcl 8.6. NaviServer 5 will be compatible with Tcl 9 (when released).

Execute the following commands on a Unix-like operating system:

```bash
$ gunzip < tcl8.6.13-src.tar.gz | tar xvf -
$ cd tcl8.6.13/unix
$ ./configure --prefix=/usr/local/ns --enable-threads --enable-symbols
$ make install
```

### 3b. Install GNU Make

If you don't have GNU make (Linux make is GNU make), install it because the server's makefiles require it. Check if you have GNU make with:

```bash
$ make -v
```

You can get GNU make from [https://www.gnu.org/](https://www.gnu.org/).

### 3c. Download, Configure, Build, and Install NaviServer

**Official releases:**  
<https://sourceforge.net/projects/naviserver/files/>

**Latest development source code (Git repository):**  
<https://github.com/naviserver-project/naviserver/>

> **Git Branches:**  
> - `main` (latest development code)  
> - `release/4.99` (bug fixes for NaviServer 4.99.*)

For official releases, run:

```bash
$ gunzip < naviserver-4.99.25.tar.gz | tar xvf -
$ cd naviserver-4.99.25
$ ./configure --prefix=/usr/local/ns --with-tcl=/usr/local/ns/lib --enable-symbols
$ make
$ su -c 'make install'
```

**Configure Script Options:**

- `--with-tcl=/usr/local/ns/lib`  
  Specifies the Tcl library installation directory where `tclConfig.sh` is located.

- `--with-zlib=/usr`  
  Specifies the path for the zlib compression library headers (e.g., `yum install zlib-devel` for Fedora). Use this if the headers are not in standard locations.

- `--enable-symbols`  
  Compile with debug symbols enabled (recommended).

- `--prefix=/usr/local/ns`  
  Set the installation directory for all program, man page, and runtime files (e.g., log files).

To compile with the Purify tool, set the variable `$PURIFY` to your Purify executable with desired options:

```bash
make PURIFY="purify -cache-dir=/home/joe/my-cache-dir" install
```

Alternatively, install NaviServer directly from GitHub using git. If you check out the source from GitHub, use `./autogen.sh` (instead of `./configure`) to generate the initial makefiles. You will need recent versions of autoconf and automake, and ensure `dtplite` (part of `tcllib`) is installed if you want to build the documentation.

> **Tip:** Run `make build-doc` to generate documentation; otherwise, `make install` will complain.

See `make help` for additional assistance.

### 3d. Create and Edit a Configuration File (nsd.tcl, by Convention)

Sample configuration files are provided:

```bash
$ cd /usr/local/ns
$ cp sample-config.tcl nsd.tcl
$ vi nsd.tcl
```

- **sample-config.tcl** contains every possible configuration option and its default value (remove any you don't need).  
- **simple-config.tcl** contains a basic set of the important configuration options (add more as needed).

### 3e. Try Running the Server in a Shell Window

```bash
$ cd /usr/local/ns
$ ./bin/nsd -f -t conf/nsd.tcl
```

The `-f` option runs the server in the foreground with important log messages directed to your terminal.

### 3f. Download and Install Additional Modules

For tar releases of NaviServer, a compatible version of the modules is provided via SourceForge. For example, to install a module named `nsfoo`:

```bash
$ gunzip < naviserver-4.99.25-modules.tar.gz | tar xvf -
$ cd modules/nsfoo
$ make install NAVISERVER=/usr/local/ns
```

Alternatively, obtain modules from GitHub:

```bash
$ git clone https://github.com/naviserver-project/nsfoo.git
$ cd nsfoo
$ make install NAVISERVER=/usr/local/ns
```

For a full list of modules, visit [https://github.com/orgs/naviserver-project/repositories](https://github.com/orgs/naviserver-project/repositories).

### 3g. Compile for Windows with Msys + Mingw

Download the minimal environment from [https://sourceforge.net/projects/mingw/files/](https://sourceforge.net/projects/mingw/files/).

1. Download the zip file and extract it.
2. Follow the instructions in `README.TXT` to launch the msys shell.
3. In the msys shell, run:

    ```bash
    $ cd /c/naviserver-4.99.25
    $ ./configure --prefix=c:/naviserver --with-tcl=c:/naviserver/lib
    $ make install
    ```

*Note:* This example assumes Tcl is built with Mingw using the prefix `c:/naviserver`.

### 3h. Compile for Windows with MSVC

Update the `tcl_64` and `tcllib_64` variables in `Makefile.win32` in the top-level NaviServer directory. Also, verify settings in `include/Makefile.win32` such as `HAVE_OPENSSL_EVP_H` and `openssl_64`.

Run the appropriate Microsoft build setup script, for example:

- `"C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"`  
- `"%ProgramFiles%\Microsoft SDKs\Windows\v7.1\Bin\SetEnv.Cmd" /Debug /x64 /win7`

Then run:

```cmd
nmake -f Makefile.win32
```

### 3i. Cross-Compiling for Windows 64-bit (using gcc/mingw)

Some settings for Mingw may not be automatically detected. Specify them explicitly:

```bash
./configure --host=x86_64-w64-mingw32 --enable-64-bit \
      --prefix=<path> --with-zlib=<path> --with-openssl=<path> --with-tcl=<path>/lib

CFLAGS="-DHAVE_INET_PTON -DHAVE_INET_NTON -DHAVE_GETADDRINFO -D_WIN32_WINNT=0x600" \
      LDFLAGS="-static-libgcc" \
      make LIBLIBS="-Wl,-Bstatic -lpthread -Wl,-Bdynamic"
```

Since the installation script does not expect the `.exe` extension, as a workaround copy the executables without the extension:

```bash
cp nsthread/nsthreadtest.exe nsthread/nsthreadtest
cp nsd/nsd.exe nsd/nsd
cp nsproxy/nsproxy.exe nsproxy/nsproxy
make install
```

### 3j. (Optional) To Compile for Windows with Cygwin

*(Instructions for Cygwin compilation would be provided here if available.)*

---

## 4. Install NSF/XOTcl

Several functions (e.g., cryptographic functions, reverse proxy functionality) depend on NSF/XOTcl. Although these components are mostly optional (except for users of Tcl 8.5), they are recommended for all installations.

Download NSF from [SourceForge](https://sourceforge.net/projects/next-scripting/) or get it from [GitHub](https://github.com/nm-wu/nsf) and install it into the NaviServer source tree using the same `--prefix` as used for NaviServer.

```bash
$ git clone https://github.com/nm-wu/nsf
$ cd nsf
$ ./configure --prefix=/usr/local/ns
$ make
$ sudo make install
```

---

## 5. Mailing Lists

There are mailing lists for NaviServer to discuss topics ranging from configuration to development and future direction. To join, visit:  
<https://sourceforge.net/projects/naviserver>

Thank you for your interest in NaviServer. We hope you find it useful and look forward to hearing from you on our mailing list.

---

