// Copyright 2015-2016 Espressif Systems (Shanghai) PTE LTD
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "vfs_api.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

using namespace fs;

#define DEFAULT_FILE_BUFFER_SIZE 4096

/* Runtime knobs (weak): firmware may provide strong defs and flip them at
   runtime to A/B the stdio workarounds below (e.g. stock FATFS behaviour
   during .srk update extraction vs the fast paths). */
extern "C" __attribute__((weak)) bool g_vfsReadBypassOn = true;
extern "C" __attribute__((weak)) uint32_t g_vfsWriteBufSize = DEFAULT_FILE_BUFFER_SIZE;

FileImplPtr VFSImpl::open(const char *fpath, const char *mode, const bool create) {
  if (!_mountpoint) {
    log_e("File system is not mounted");
    return FileImplPtr();
  }

  if (!fpath || fpath[0] != '/') {
    log_e("%s does not start with /", fpath);
    return FileImplPtr();
  }

  char *temp = (char *)malloc(strlen(fpath) + strlen(_mountpoint) + 2);
  if (!temp) {
    log_e("malloc failed");
    return FileImplPtr();
  }

  strcpy(temp, _mountpoint);
  strcat(temp, fpath);

  // Try to open as file first - let the file operation handle errors
  if (mode && mode[0] != 'r') {
    // For write modes, attempt to create directories if needed
    if (create) {
      char *token;
      char *folder = (char *)malloc(strlen(fpath) + 1);

      int start_index = 0;
      int end_index = 0;

      token = strchr(fpath + 1, '/');
      end_index = (token - fpath);

      while (token != NULL) {
        memcpy(folder, fpath + start_index, end_index - start_index);
        folder[end_index - start_index] = '\0';

        if (!VFSImpl::mkdir(folder)) {
          log_e("Creating folder: %s failed!", folder);
          free(folder);
          free(temp);
          return FileImplPtr();
        }

        token = strchr(token + 1, '/');
        if (token != NULL) {
          end_index = (token - fpath);
          memset(folder, 0, strlen(folder));
        }
      }

      free(folder);
    }

    // Try to open the file directly - let fopen handle errors
    free(temp);
    return std::make_shared<VFSFileImpl>(this, fpath, mode);
  }

  // For read mode, let the VFSFileImpl constructor handle the file opening
  // This avoids the TOCTOU race condition while maintaining proper functionality
  free(temp);
  return std::make_shared<VFSFileImpl>(this, fpath, mode);
}

bool VFSImpl::exists(const char *fpath) {
  if (!_mountpoint) {
    log_e("File system is not mounted");
    return false;
  }

  VFSFileImpl f(this, fpath, "r");
  if (f) {
    f.close();
    return true;
  }
  return false;
}

bool VFSImpl::rename(const char *pathFrom, const char *pathTo) {
  if (!_mountpoint) {
    log_e("File system is not mounted");
    return false;
  }

  if (!pathFrom || pathFrom[0] != '/' || !pathTo || pathTo[0] != '/') {
    log_e("bad arguments");
    return false;
  }

  size_t mountpointLen = strlen(_mountpoint);
  char *temp1 = (char *)malloc(strlen(pathFrom) + mountpointLen + 1);
  if (!temp1) {
    log_e("malloc failed");
    return false;
  }
  char *temp2 = (char *)malloc(strlen(pathTo) + mountpointLen + 1);
  if (!temp2) {
    free(temp1);
    log_e("malloc failed");
    return false;
  }

  strcpy(temp1, _mountpoint);
  strcat(temp1, pathFrom);

  strcpy(temp2, _mountpoint);
  strcat(temp2, pathTo);

  // Let rename() handle the error if source doesn't exist
  auto rc = ::rename(temp1, temp2);
  free(temp1);
  free(temp2);
  return rc == 0;
}

bool VFSImpl::remove(const char *fpath) {
  if (!_mountpoint) {
    log_e("File system is not mounted");
    return false;
  }

  if (!fpath || fpath[0] != '/') {
    log_e("bad arguments");
    return false;
  }

  char *temp = (char *)malloc(strlen(fpath) + strlen(_mountpoint) + 1);
  if (!temp) {
    log_e("malloc failed");
    return false;
  }

  strcpy(temp, _mountpoint);
  strcat(temp, fpath);

  // Let unlink() handle the error if file doesn't exist
  auto rc = unlink(temp);
  free(temp);
  return rc == 0;
}

bool VFSImpl::mkdir(const char *fpath) {
  if (!_mountpoint) {
    log_e("File system is not mounted");
    return false;
  }

  VFSFileImpl f(this, fpath, "r");
  if (f && f.isDirectory()) {
    f.close();
    //log_w("%s already exists", fpath);
    return true;
  } else if (f) {
    f.close();
    log_e("%s is a file", fpath);
    return false;
  }

  char *temp = (char *)malloc(strlen(fpath) + strlen(_mountpoint) + 1);
  if (!temp) {
    log_e("malloc failed");
    return false;
  }

  strcpy(temp, _mountpoint);
  strcat(temp, fpath);

  auto rc = ::mkdir(temp, ACCESSPERMS);
  free(temp);
  return rc == 0;
}

bool VFSImpl::rmdir(const char *fpath) {
  if (!_mountpoint) {
    log_e("File system is not mounted");
    return false;
  }

  if (strcmp(_mountpoint, "/spiffs") == 0) {
    log_e("rmdir is unnecessary in SPIFFS");
    return false;
  }

  char *temp = (char *)malloc(strlen(fpath) + strlen(_mountpoint) + 1);
  if (!temp) {
    log_e("malloc failed");
    return false;
  }

  strcpy(temp, _mountpoint);
  strcat(temp, fpath);

  // Let rmdir() handle the error if directory doesn't exist
  auto rc = ::rmdir(temp);
  free(temp);
  return rc == 0;
}

VFSFileImpl::VFSFileImpl(VFSImpl *fs, const char *fpath, const char *mode) : _fs(fs), _f(NULL), _d(NULL), _path(NULL), _isDirectory(false), _written(false) {
  char *temp = (char *)malloc(strlen(fpath) + strlen(_fs->_mountpoint) + 1);
  if (!temp) {
    return;
  }

  strcpy(temp, _fs->_mountpoint);
  strcat(temp, fpath);

  _path = strdup(fpath);
  if (!_path) {
    log_e("strdup(%s) failed", fpath);
    free(temp);
    return;
  }

  // For read mode, check if file exists first to determine type
  if (!mode || mode[0] == 'r') {
    if (!stat(temp, &_stat)) {
      //file found
      if (S_ISREG(_stat.st_mode)) {
        _isDirectory = false;
        _f = fopen(temp, mode);
        if (!_f) {
          log_e("fopen(%s) failed", temp);
        }
        if (_f && (_stat.st_blksize == 0) && g_vfsWriteBufSize >= 1024) {
          setvbuf(_f, NULL, _IOFBF, (int)g_vfsWriteBufSize);
        }
      } else if (S_ISDIR(_stat.st_mode)) {
        _isDirectory = true;
        _d = opendir(temp);
        if (!_d) {
          log_e("opendir(%s) failed", temp);
        }
      } else {
        log_e("Unknown type 0x%08X for file %s", ((_stat.st_mode) & _IFMT), temp);
      }
    } else {
      //file not found
      //try to open as directory
      _d = opendir(temp);
      if (_d) {
        _isDirectory = true;
      } else {
        _isDirectory = false;
        //log_w("stat(%s) failed", temp);
      }
    }
  } else {
    //lets create this new file
    _isDirectory = false;
    _f = fopen(temp, mode);
    if (!_f) {
      log_e("fopen(%s) failed", temp);
    }
    if (!stat(temp, &_stat)) {
      if (_f && (_stat.st_blksize == 0)) {
        setvbuf(_f, NULL, _IOFBF, DEFAULT_FILE_BUFFER_SIZE);
      }
    } else {
      log_e("stat(%s) failed", temp);
    }
  }
  free(temp);
}

VFSFileImpl::~VFSFileImpl() {
  close();
}

void VFSFileImpl::close() {
  if (_path) {
    free(_path);
    _path = NULL;
  }
  if (_isDirectory && _d) {
    closedir(_d);
    _d = NULL;
    _isDirectory = false;
  } else if (_f) {
    fclose(_f);
    _f = NULL;
  }
}

VFSFileImpl::operator bool() {
  return (_isDirectory && _d != NULL) || _f != NULL;
}

time_t VFSFileImpl::getLastWrite() {
  _getStat();
  return _stat.st_mtime;
}

void VFSFileImpl::_getStat() const {
  if (!_path) {
    return;
  }
  char *temp = (char *)malloc(strlen(_path) + strlen(_fs->_mountpoint) + 1);
  if (!temp) {
    return;
  }

  strcpy(temp, _fs->_mountpoint);
  strcat(temp, _path);

  if (!stat(temp, &_stat)) {
    _written = false;
  }
  free(temp);
}

size_t VFSFileImpl::write(const uint8_t *buf, size_t size) {
  if (_isDirectory || !_f || !buf || !size) {
    return 0;
  }
  _written = true;
  return fwrite(buf, 1, size, _f);
}

size_t VFSFileImpl::read(uint8_t *buf, size_t size) {
  if (_isDirectory || !_f || !buf || !size) {
    return 0;
  }

  /* newlib-nano's fread refills the stdio buffer in 1 KB chunks (BUFSIZ)
     regardless of the configured buffer size. On SD-backed files every
     1 KB refill becomes a tiny 2-sector SD command (~1 ms per-command
     overhead) -> ~840 KB/s ceiling instead of the ~8 MB/s the bus can do.
     For bulk reads (>= 1 KB) use pread() instead: it seeks at the FATFS
     level (vfs_fat_pread), reads, and RESTORES the position -- the fd
     never moves and newlib's FILE bookkeeping is never invalidated, so a
     plain fseek(SEEK_CUR) afterwards advances the logical position
     correctly. (An earlier ::read() bypass moved the fd behind newlib's
     back and scrambled reads that followed buffered small reads -- that
     bug destroyed .srk index reads: 12-byte header fread filled 1KB of
     read-ahead, the raw read then started 1KB late.)
     Small reads (byte-wise JSON parsing etc.) stay on the buffered path
     where they usually cost zero syscalls from the warm stdio buffer. */
   if (size >= 1024 && g_vfsReadBypassOn) {
    long pos = ftell(_f);
    if (pos < 0) {
      return fread(buf, 1, size, _f);
    }
    ssize_t n = pread(fileno(_f), buf, size, (off_t)pos);
    if (n <= 0) {
      return 0;
    }
    if (fseek(_f, (long)n, SEEK_CUR) != 0) {
      fseek(_f, pos + n, SEEK_SET); /* absolute resync fallback */
    }
    return n;
  }
  return fread(buf, 1, size, _f);
}

void VFSFileImpl::flush() {
  if (_isDirectory || !_f) {
    return;
  }
  fflush(_f);
  // workaround for https://github.com/espressif/arduino-esp32/issues/1293
  fsync(fileno(_f));
}

bool VFSFileImpl::seek(uint32_t pos, SeekMode mode) {
  if (_isDirectory || !_f) {
    return false;
  }
  auto rc = fseek(_f, pos, mode);
  return rc == 0;
}

size_t VFSFileImpl::position() const {
  if (_isDirectory || !_f) {
    return 0;
  }
  return ftell(_f);
}

size_t VFSFileImpl::size() const {
  if (_isDirectory || !_f) {
    return 0;
  }
  if (_written) {
    _getStat();
  }
  return _stat.st_size;
}

/*
* Change size of files internal buffer used for read / write operations.
* Need to be called right after opening file before any other operation!
*/
bool VFSFileImpl::setBufferSize(size_t size) {
  if (_isDirectory || !_f) {
    return 0;
  }
  int res = setvbuf(_f, NULL, _IOFBF, size);
  return res == 0;
}

const char *VFSFileImpl::path() const {
  return (const char *)_path;
}

const char *VFSFileImpl::name() const {
  return pathToFileName(path());
}

//to implement
boolean VFSFileImpl::isDirectory(void) {
  return _isDirectory;
}

FileImplPtr VFSFileImpl::openNextFile(const char *mode) {
  if (!_isDirectory || !_d) {
    return FileImplPtr();
  }
  struct dirent *file = readdir(_d);
  if (file == NULL) {
    return FileImplPtr();
  }
  if (file->d_type != DT_REG && file->d_type != DT_DIR) {
    return openNextFile(mode);
  }

  size_t pathLen = strlen(_path);
  size_t fileNameLen = strlen(file->d_name);
  char *name = (char *)malloc(pathLen + fileNameLen + 2);

  if (name == NULL) {
    return FileImplPtr();
  }

  strcpy(name, _path);

  if ((file->d_name[0] != '/') && (_path[pathLen - 1] != '/')) {
    strcat(name, "/");
  }

  strcat(name, file->d_name);

  FileImplPtr fileImplPtr = std::make_shared<VFSFileImpl>(_fs, name, mode);
  free(name);
  return fileImplPtr;
}

boolean VFSFileImpl::seekDir(long position) {
  if (!_d) {
    return false;
  }
  seekdir(_d, position);
  return true;
}

String VFSFileImpl::getNextFileName() {
  if (!_isDirectory || !_d) {
    return "";
  }
  struct dirent *file = readdir(_d);
  if (file == NULL) {
    return "";
  }
  if (file->d_type != DT_REG && file->d_type != DT_DIR) {
    return "";
  }
  String fname = String(file->d_name);
  String name = String(_path);
  if (!fname.startsWith("/") && !name.endsWith("/")) {
    name += "/";
  }
  name += fname;
  return name;
}

String VFSFileImpl::getNextFileName(bool *isDir) {
  if (!_isDirectory || !_d) {
    return "";
  }
  struct dirent *file = readdir(_d);
  if (file == NULL) {
    return "";
  }
  if (file->d_type != DT_REG && file->d_type != DT_DIR) {
    return "";
  }
  String fname = String(file->d_name);
  String name = String(_path);
  if (!fname.startsWith("/") && !name.endsWith("/")) {
    name += "/";
  }
  name += fname;

  // check entry is a directory
  if (isDir) {
    *isDir = (file->d_type == DT_DIR);
  }
  return name;
}

void VFSFileImpl::rewindDirectory(void) {
  if (!_isDirectory || !_d) {
    return;
  }
  rewinddir(_d);
}
