# wmic
- Linux wmi 1.3.16 taken from wget http://www.opsview.com/sites/default/files/wmi-1.3.16.tar_.bz2  and patched. 
- **Ready to compile**
- In Samba/source/pidl/pidl line 583 I already removed the word defined before @$pidl

### How to Compile
- Tested on Debian 9 Stretch
- cd /usr/src/
- apt-get install autoconf gcc libdatetime-perl make build-essential g++ python-dev
- git clone git@github.com:astbss/wmic.git
- cd /usr/src/wmic
- make "CPP=gcc -E -ffreestanding",
- cp Samba/source/bin/wmic /usr/local/bin/

### How to Test it
- **Get system information**
- wmic -U administrator --password=very-secure-password //78.46.41.48 "SELECT * FROM Win32_OperatingSystem"

- **Get list of running processes**
- wmic -U administrator --password=very-secure-password //78.46.41.48 "select caption, name, parentprocessid, processid from win32_process"
