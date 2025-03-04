/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "perfetto/ext/base/file_utils.h"

#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <deque>
#include <string>
#include <vector>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/platform_handle.h"
#include "perfetto/base/status.h"
#include "perfetto/ext/base/scoped_file.h"
#include "perfetto/ext/base/utils.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
#include <Windows.h>
#include <direct.h>
#include <io.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

namespace perfetto {
namespace base {
namespace {
constexpr size_t kBufSize = 2048;
}  // namespace

ssize_t Read(int fd, void* dst, size_t dst_size) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return _read(fd, dst, static_cast<unsigned>(dst_size));
#else
  return PERFETTO_EINTR(read(fd, dst, dst_size));
#endif
}

bool ReadFileDescriptor(int fd, std::string* out) {
  // Do not override existing data in string.
  size_t i = out->size();

  struct stat buf {};
  if (fstat(fd, &buf) != -1) {
    if (buf.st_size > 0)
      out->resize(i + static_cast<size_t>(buf.st_size));
  }

  ssize_t bytes_read;
  for (;;) {
    if (out->size() < i + kBufSize)
      out->resize(out->size() + kBufSize);

    bytes_read = Read(fd, &((*out)[i]), kBufSize);
    if (bytes_read > 0) {
      i += static_cast<size_t>(bytes_read);
    } else {
      out->resize(i);
      return bytes_read == 0;
    }
  }
}

bool ReadPlatformHandle(PlatformHandle h, std::string* out) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // Do not override existing data in string.
  size_t i = out->size();

  for (;;) {
    if (out->size() < i + kBufSize)
      out->resize(out->size() + kBufSize);
    DWORD bytes_read = 0;
    auto res = ::ReadFile(h, &((*out)[i]), kBufSize, &bytes_read, nullptr);
    if (res && bytes_read > 0) {
      i += static_cast<size_t>(bytes_read);
    } else {
      out->resize(i);
      const bool is_eof = res && bytes_read == 0;
      auto err = res ? 0 : GetLastError();
      // The "Broken pipe" error on Windows is slighly different than Unix:
      // On Unix: a "broken pipe" error can happen only on the writer side. On
      // the reader there is no broken pipe, just a EOF.
      // On windows: the reader also sees a broken pipe error.
      // Here we normalize on the Unix behavior, treating broken pipe as EOF.
      return is_eof || err == ERROR_BROKEN_PIPE;
    }
  }
#else
  return ReadFileDescriptor(h, out);
#endif
}

bool ReadFileStream(FILE* f, std::string* out) {
  return ReadFileDescriptor(fileno(f), out);
}

bool ReadFile(const std::string& path, std::string* out) {
  base::ScopedFile fd = base::OpenFile(path, O_RDONLY);
  if (!fd)
    return false;

  return ReadFileDescriptor(*fd, out);
}

ssize_t WriteAll(int fd, const void* buf, size_t count) {
  size_t written = 0;
  while (written < count) {
    // write() on windows takes an unsigned int size.
    uint32_t bytes_left = static_cast<uint32_t>(
        std::min(count - written, static_cast<size_t>(UINT32_MAX)));
    ssize_t wr = PERFETTO_EINTR(
        write(fd, static_cast<const char*>(buf) + written, bytes_left));
    if (wr == 0)
      break;
    if (wr < 0)
      return wr;
    written += static_cast<size_t>(wr);
  }
  return static_cast<ssize_t>(written);
}

ssize_t WriteAllHandle(PlatformHandle h, const void* buf, size_t count) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  DWORD wsize = 0;
  if (::WriteFile(h, buf, static_cast<DWORD>(count), &wsize, nullptr)) {
    return wsize;
  } else {
    return -1;
  }
#else
  return WriteAll(h, buf, count);
#endif
}

bool FlushFile(int fd) {
  PERFETTO_DCHECK(fd != 0);
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  return !PERFETTO_EINTR(fdatasync(fd));
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return !PERFETTO_EINTR(_commit(fd));
#else
  return !PERFETTO_EINTR(fsync(fd));
#endif
}

bool Mkdir(const std::string& path) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return _mkdir(path.c_str()) == 0;
#else
  return mkdir(path.c_str(), 0755) == 0;
#endif
}

bool Rmdir(const std::string& path) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return _rmdir(path.c_str()) == 0;
#else
  return rmdir(path.c_str()) == 0;
#endif
}

int CloseFile(int fd) {
  return close(fd);
}

ScopedFile OpenFile(const std::string& path, int flags, FileOpenMode mode) {
  PERFETTO_DCHECK((flags & O_CREAT) == 0 || mode != kFileModeInvalid);
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // Always use O_BINARY on Windows, to avoid silly EOL translations.
  ScopedFile fd(_open(path.c_str(), flags | O_BINARY, mode));
#else
  // Always open a ScopedFile with O_CLOEXEC so we can safely fork and exec.
  ScopedFile fd(open(path.c_str(), flags | O_CLOEXEC, mode));
#endif
  return fd;
}

bool FileExists(const std::string& path) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  return _access(path.c_str(), 0) == 0;
#else
  return access(path.c_str(), F_OK) == 0;
#endif
}

// Declared in base/platform_handle.h.
int ClosePlatformHandle(PlatformHandle handle) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  // Make the return value UNIX-style.
  return CloseHandle(handle) ? 0 : -1;
#else
  return close(handle);
#endif
}

base::Status ListFilesRecursive(const std::string& dir_path,
                                std::vector<std::string>& output) {
  std::string root_dir_path = dir_path;
  if (root_dir_path.back() == '\\') {
    root_dir_path.back() = '/';
  } else if (root_dir_path.back() != '/') {
    root_dir_path.push_back('/');
  }

  // dir_queue contains full paths to the directories. The paths include the
  // root_dir_path at the beginning and the trailing slash at the end.
  std::deque<std::string> dir_queue;
  dir_queue.push_back(root_dir_path);

  while (!dir_queue.empty()) {
    const std::string cur_dir = std::move(dir_queue.front());
    dir_queue.pop_front();
#if PERFETTO_BUILDFLAG(PERFETTO_OS_NACL)
    return base::ErrStatus("ListFilesRecursive not supported yet");
#elif PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
    std::string glob_path = cur_dir + "*";
    // + 1 because we also have to count the NULL terminator.
    if (glob_path.length() + 1 > MAX_PATH)
      return base::ErrStatus("Directory path %s is too long", dir_path.c_str());
    WIN32_FIND_DATAA ffd;
    // We do not use a ScopedResource for the HANDLE from FindFirstFile because
    // the invalid value INVALID_HANDLE_VALUE is not a constexpr under some
    // compile configurations, and thus cannot be used as a template argument.
    HANDLE hFind = FindFirstFileA(glob_path.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
      // For empty directories, there should be at least one entry '.'.
      // If FindFirstFileA returns INVALID_HANDLE_VALUE, this means directory
      // couldn't be accessed.
      FindClose(hFind);
      return base::ErrStatus("Failed to open directory %s", cur_dir.c_str());
    }
    do {
      if (strcmp(ffd.cFileName, ".") == 0 || strcmp(ffd.cFileName, "..") == 0)
        continue;
      if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        std::string subdir_path = cur_dir + ffd.cFileName + '/';
        dir_queue.push_back(subdir_path);
      } else if (ffd.dwFileAttributes & FILE_ATTRIBUTE_NORMAL) {
        const std::string full_path = cur_dir + ffd.cFileName;
        PERFETTO_CHECK(full_path.length() > root_dir_path.length());
        output.push_back(full_path.substr(root_dir_path.length()));
      }
    } while (FindNextFileA(hFind, &ffd));
    FindClose(hFind);
#else
    ScopedDir dir = ScopedDir(opendir(cur_dir.c_str()));
    if (!dir) {
      return base::ErrStatus("Failed to open directory %s", cur_dir.c_str());
    }
    for (auto* dirent = readdir(dir.get()); dirent != nullptr;
         dirent = readdir(dir.get())) {
      if (strcmp(dirent->d_name, ".") == 0 ||
          strcmp(dirent->d_name, "..") == 0) {
        continue;
      }
      if (dirent->d_type == DT_DIR) {
        dir_queue.push_back(cur_dir + dirent->d_name + '/');
      } else if (dirent->d_type == DT_REG) {
        const std::string full_path = cur_dir + dirent->d_name;
        PERFETTO_CHECK(full_path.length() > root_dir_path.length());
        output.push_back(full_path.substr(root_dir_path.length()));
      }
    }
#endif
  }
  return base::OkStatus();
}

std::string GetFileExtension(const std::string& filename) {
  auto ext_idx = filename.rfind('.');
  if (ext_idx == std::string::npos)
    return std::string();
  return filename.substr(ext_idx);
}

base::Optional<size_t> GetFileSize(const std::string& file_path) {
#if PERFETTO_BUILDFLAG(PERFETTO_OS_WIN)
  HANDLE file =
      CreateFileA(file_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return nullopt;
  }
  LARGE_INTEGER file_size;
  file_size.QuadPart = 0;
  BOOL ok = GetFileSizeEx(file, &file_size);
  CloseHandle(file);
  if (!ok) {
    return nullopt;
  }
  return static_cast<size_t>(file_size.QuadPart);
#else
  base::ScopedFile fd(base::OpenFile(file_path, O_RDONLY | O_CLOEXEC));
  if (!fd) {
    return nullopt;
  }
  struct stat buf{};
  if (fstat(*fd, &buf) == -1) {
    return nullopt;
  }
  return static_cast<size_t>(buf.st_size);
#endif
}

}  // namespace base
}  // namespace perfetto
