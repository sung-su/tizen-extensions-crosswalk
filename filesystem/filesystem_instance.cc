// Copyright (c) 2014 Intel Corporation. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "filesystem/filesystem_instance.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <iconv.h>
#include <pkgmgr-info.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <iostream>
#include <sstream>
#include <utility>

namespace {
const unsigned kDefaultFileMode = 0755;
const char kDefaultPath[] = "/opt/usr/media";
const char kPathCamera[] = "/opt/usr/media/Camera";
const char kPathSounds[] = "/opt/usr/media/Sounds";
const char kPathImages[] = "/opt/usr/media/Images";
const char kPathVideos[] = "/opt/usr/media/Videos";
const char kPathDownloads[] = "/opt/usr/media/Downloads";
const char kPathDocuments[] = "/opt/usr/media/Documents";
const char kPathRingtones[] = "/opt/usr/share/settings/Ringtones";

const char kLocationCamera[] = "camera";
const char kLocationMusic[] = "music";
const char kLocationImages[] = "images";
const char kLocationVideos[] = "videos";
const char kLocationDownloads[] = "downloads";
const char kLocationDocuments[] = "documents";
const char kLocationRingtones[] = "ringtones";
const char kLocationWgtPackage[] = "wgt-package";
const char kLocationWgtPrivate[] = "wgt-private";
const char kLocationWgtPrivateTmp[] = "wgt-private-tmp";

const char kInternalStorage[] = "internal";
const char kRemovableStorage[] = "removable";

const char kStorageTypeInternal[] = "INTERNAL";
const char kStorageTypeExternal[] = "EXTERNAL";
const char kStorageStateMounted[] = "MOUNTED";
const char kStorageStateRemoved[] = "REMOVED";
const char kStorageStateUnmountable[] = "UNMOUNTABLE";

unsigned int lastStreamId = 0;
// FIXME(ricardotk): This needs another approach, kMaxSize is a palliative
// solution.
const unsigned kMaxSize = 64 * 1024;

bool IsWritable(const struct stat& st) {
  if (st.st_mode & S_IWOTH)
    return true;
  if ((st.st_mode & S_IWUSR) && geteuid() == st.st_uid)
    return true;
  if ((st.st_mode & S_IWGRP) && getegid() == st.st_gid)
    return true;
  return false;
}

std::string JoinPath(const std::string& one, const std::string& another) {
  return one + '/' + another;
}

// This function creates full path and parent directories when needed.
// Similar to "mkdir -p".
bool makePath(const std::string& path) {
  // Path should start with '/' and contain at least 1 character after '/'.
  if (path.empty() || path[0] != '/' || path.length() < 2)
    return false;

  struct stat st;
  std::string dir = path;
  if (stat(dir.c_str(), &st) == 0)
    return true;

  // Add trailing '/' if missing, so we can iterate till the end of the path.
  if (dir[dir.size() - 1] != '/')
    dir += '/';

  for (auto iter = dir.begin(); iter != dir.end();) {
    auto cur_iter = std::find(iter, dir.end(), '/');

    // If '/' is found at the beginning of the string, iterate to the next '/'.
    if (cur_iter == iter) {
      ++iter;
      cur_iter = std::find(iter, dir.end(), '/');
    }

    std::string new_path = std::string(dir.begin(), cur_iter);

    // If path doesn't exist, try to create one and continue iteration.
    // In case of error, stop iteration and return.
    if (stat(new_path.c_str(), &st) != 0) {
      if (mkdir(new_path.c_str(), kDefaultFileMode) != 0 && errno != EEXIST )
          return false;
    // If path exists and it is not a directory, stop iteration and return.
    } else if (!S_ISDIR(st.st_mode)) {
      return false;
    }

    // Advance iterator and create next parent folder.
    iter = cur_iter;
    if (cur_iter != dir.end())
      ++iter;
  }
  return true;
}

int get_dir_entry_count(const char* path) {
  int count = 0;
  DIR* dir = opendir(path);
  if (!dir)
    return count;

  struct dirent entry;
  struct dirent *result;
  int ret = readdir_r(dir, &entry, &result);

  for (; ret == 0 && result != NULL; ret = readdir_r(dir, &entry, &result)) {
    if (entry.d_type == DT_REG || entry.d_type == DT_DIR)
      count++;
  }

  closedir(dir);
  return count;
}

std::string GetAppId(const std::string& package_id) {
  char *appid = NULL;
  pkgmgrinfo_pkginfo_h pkginfo_handle;
  int ret = pkgmgrinfo_pkginfo_get_pkginfo(package_id.c_str(), &pkginfo_handle);
  if (ret != PMINFO_R_OK)
    return std::string();
  ret = pkgmgrinfo_pkginfo_get_mainappid(pkginfo_handle, &appid);
  if (ret != PMINFO_R_OK) {
    pkgmgrinfo_pkginfo_destroy_pkginfo(pkginfo_handle);
    return std::string();
  }

  std::string retval(appid);
  pkgmgrinfo_pkginfo_destroy_pkginfo(pkginfo_handle);
  return retval;
}

std::string GetExecPath(const std::string& app_id) {
  char* exec_path = NULL;
  pkgmgrinfo_appinfo_h appinfo_handle;
  int ret = pkgmgrinfo_appinfo_get_appinfo(app_id.c_str(), &appinfo_handle);
  if (ret != PMINFO_R_OK)
    return std::string();
  ret = pkgmgrinfo_appinfo_get_exec(appinfo_handle, &exec_path);
  if (ret != PMINFO_R_OK) {
    pkgmgrinfo_appinfo_destroy_appinfo(appinfo_handle);
    return std::string();
  }

  std::string retval(exec_path);
  pkgmgrinfo_appinfo_destroy_appinfo(appinfo_handle);
  return retval;
}

std::string GetApplicationPath() {
  std::string id_str = common::Extension::GetRuntimeVariable("app_id", 64);
  picojson::value id_val;
  std::istringstream buf(id_str);
  std::string error = picojson::parse(id_val, buf);
  if (!error.empty()) {
    std::cerr << "Got invalid package ID." << std::endl;
    return std::string();
  }

  std::string pkg_id = id_val.get<std::string>();
  if (pkg_id.empty())
    return std::string();

  std::string app_id = GetAppId(pkg_id);
  if (app_id.empty())
    return std::string();
  std::string exec_path = GetExecPath(app_id);
  if (exec_path.empty())
    return std::string();

  size_t index = exec_path.find(pkg_id);
  if (index != std::string::npos)
    return exec_path.substr(0, index + pkg_id.length());
  return std::string();
}

};  // namespace

FilesystemInstance::FilesystemInstance() {
  std::string app_path = GetApplicationPath();
  if (!app_path.empty()) {
    AddInternalStorage(kLocationWgtPackage, app_path);
    AddInternalStorage(kLocationWgtPrivate, JoinPath(app_path, "private"));
    AddInternalStorage(kLocationWgtPrivateTmp, JoinPath(app_path, "tmp"));
  }

  AddInternalStorage(kLocationCamera, kPathCamera);
  AddInternalStorage(kLocationMusic, kPathSounds);
  AddInternalStorage(kLocationImages, kPathImages);
  AddInternalStorage(kLocationVideos, kPathVideos);
  AddInternalStorage(kLocationDownloads, kPathDownloads);
  AddInternalStorage(kLocationDocuments, kPathDocuments);
  AddInternalStorage(kLocationRingtones, kPathRingtones);
}

FilesystemInstance::~FilesystemInstance() {
  FStreamMap::iterator it;

  for (it = fstream_map_.begin(); it != fstream_map_.end(); it++) {
    std::fstream* fs = it->second.second;
    fs->close();
    delete(fs);
  }
}

void FilesystemInstance::HandleMessage(const char* message) {
  picojson::value v;

  std::string err;
  picojson::parse(v, message, message + strlen(message), &err);
  if (!err.empty()) {
    std::cout << "Ignoring message.\n";
    return;
  }

  std::string cmd = v.get("cmd").to_str();
  if (cmd == "FileSystemManagerResolve")
    HandleFileSystemManagerResolve(v);
  else if (cmd == "FileSystemManagerGetStorage")
    HandleFileSystemManagerGetStorage(v);
  else if (cmd == "FileSystemManagerListStorages")
    HandleFileSystemManagerListStorages(v);
  else if (cmd == "FileOpenStream")
    HandleFileOpenStream(v);
  else if (cmd == "FileDeleteDirectory")
    HandleFileDeleteDirectory(v);
  else if (cmd == "FileDeleteFile")
    HandleFileDeleteFile(v);
  else if (cmd == "FileListFiles")
    HandleFileListFiles(v);
  else if (cmd == "FileCopyTo")
    HandleFileCopyTo(v);
  else if (cmd == "FileMoveTo")
    HandleFileMoveTo(v);
  else
    std::cout << "Ignoring unknown command: " << cmd;
}

void FilesystemInstance::PostAsyncErrorReply(const picojson::value& msg,
      WebApiAPIErrors error_code) {
  picojson::value::object o;
  o["isError"] = picojson::value(true);
  o["errorCode"] = picojson::value(static_cast<double>(error_code));
  o["reply_id"] = picojson::value(msg.get("reply_id").get<double>());

  picojson::value v(o);
  PostMessage(v.serialize().c_str());
}

void FilesystemInstance::PostAsyncSuccessReply(const picojson::value& msg,
      picojson::value::object& reply) {
  reply["isError"] = picojson::value(false);
  reply["reply_id"] = picojson::value(msg.get("reply_id").get<double>());

  picojson::value v(reply);
  PostMessage(v.serialize().c_str());
}

void FilesystemInstance::PostAsyncSuccessReply(const picojson::value& msg) {
  picojson::value::object reply;
  PostAsyncSuccessReply(msg, reply);
}

void FilesystemInstance::PostAsyncSuccessReply(const picojson::value& msg,
      picojson::value& value) {
  picojson::value::object reply;
  reply["value"] = value;
  PostAsyncSuccessReply(msg, reply);
}

void FilesystemInstance::HandleFileSystemManagerResolve(
      const picojson::value& msg) {
  if (!msg.contains("location")) {
    PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
    return;
  }

  std::string location = msg.get("location").to_str();
  bool check_if_inside_default = true;
  std::string mode;

  mode = msg.contains("mode") ? msg.get("mode").to_str() : "rw";

  size_t pos_wgt_pkg = location.find(kLocationWgtPackage);
  size_t pos_ringtones = location.find(kLocationRingtones);

  if (pos_wgt_pkg != std::string::npos || pos_ringtones != std::string::npos) {
    if (mode == "w" || mode == "rw" || mode == "a") {
      PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
      return;
    }
    mode = "r";
  }

  if (pos_wgt_pkg != std::string::npos ||
      pos_ringtones != std::string::npos ||
      location.find(kLocationWgtPrivate) != std::string::npos)
    check_if_inside_default = false;

  std::string real_path;
  if (location.find("file://") == 0) {
    real_path = location.substr(sizeof("file://") - 1);
    check_if_inside_default = false;
  } else {
    real_path = GetRealPath(location);
  }

  if (real_path.empty()) {
    PostAsyncErrorReply(msg, NOT_FOUND_ERR);
    return;
  }

  char* real_path_cstr = realpath(real_path.c_str(), NULL);
  if (!real_path_cstr) {
    PostAsyncErrorReply(msg, IO_ERR);
    return;
  }
  std::string real_path_ack = std::string(real_path_cstr);
  free(real_path_cstr);

  if (check_if_inside_default && real_path_ack.find(kDefaultPath) != 0) {
    PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
    return;
  }

  struct stat st;
  if (stat(real_path_ack.c_str(), &st) < 0) {
    if (errno == ENOENT || errno == ENOTDIR)
      PostAsyncErrorReply(msg, NOT_FOUND_ERR);
    else
      PostAsyncErrorReply(msg, IO_ERR);
    return;
  }

  if (!IsWritable(st) && (mode == "w" || mode == "rw")) {
    PostAsyncErrorReply(msg, IO_ERR);
    return;
  }

  picojson::value::object o;
  o["fullPath"] = picojson::value(location);
  PostAsyncSuccessReply(msg, o);
}

void FilesystemInstance::HandleFileSystemManagerGetStorage(
      const picojson::value& msg) {
  storage_foreach_device_supported(OnStorageDeviceSupported, this);
  Storages::const_iterator it = storages_.find(msg.get("label").to_str());

  if (it == storages_.end()) {
    PostAsyncErrorReply(msg, NOT_FOUND_ERR);
    return;
  }

  picojson::object storage_object = it->second.toJSON(it->first);
  PostAsyncSuccessReply(msg, storage_object);
}

void FilesystemInstance::HandleFileSystemManagerListStorages(
      const picojson::value& msg) {
  storage_foreach_device_supported(OnStorageDeviceSupported, this);
  picojson::array storage_objects;
  Storages::const_iterator it = storages_.begin();
  while (it != storages_.end()) {
    picojson::object storage_object = it->second.toJSON(it->first);
    storage_objects.push_back(picojson::value(storage_object));
    ++it;
  }

  picojson::value value(storage_objects);
  PostAsyncSuccessReply(msg, value);
}

void FilesystemInstance::HandleFileOpenStream(const picojson::value& msg) {
  if (!msg.contains("mode")) {
    PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
    return;
  }

  std::string mode = msg.get("mode").to_str();
  std::ios_base::openmode open_mode = std::ios_base::binary;
  if (mode == "a") {
    open_mode |= (std::ios_base::app | std::ios_base::out);
  } else if (mode == "w") {
    open_mode |= std::ios_base::out;
  } else if (mode == "rw") {
    open_mode |= (std::ios_base::in | std::ios_base::out);
  } else if (mode == "r") {
    open_mode |= std::ios_base::in;
  } else {
    PostAsyncErrorReply(msg, TYPE_MISMATCH_ERR);
    return;
  }

  std::string encoding = "";
  if (msg.contains("encoding"))
    encoding = msg.get("encoding").to_str();
  if (!encoding.empty() && (encoding != "UTF-8" && encoding != "ISO-8859-1")) {
    PostAsyncErrorReply(msg, TYPE_MISMATCH_ERR);
    return;
  }

  if (!msg.contains("fullPath")) {
    PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
    return;
  }

  std::string real_path = GetRealPath(msg.get("fullPath").to_str());
  char* real_path_cstr = realpath(real_path.c_str(), NULL);
  if (!real_path_cstr) {
    free(real_path_cstr);
    PostAsyncErrorReply(msg, IO_ERR);
    return;
  }

  struct stat st;
  if (stat(real_path_cstr, &st) < 0) {
    free(real_path_cstr);
    PostAsyncErrorReply(msg, IO_ERR);
    return;
  }

  if (S_ISDIR(st.st_mode)) {
    free(real_path_cstr);
    PostAsyncErrorReply(msg, IO_ERR);
    return;
  }

  std::fstream* fs = new std::fstream(real_path_cstr, open_mode);
  if (!(*fs) || !fs->is_open()) {
    free(real_path_cstr);
    PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
    return;
  }
  free(real_path_cstr);

  fstream_map_[lastStreamId] = FStream(open_mode, fs);

  picojson::value::object o;
  o["streamID"] = picojson::value(static_cast<double>(lastStreamId));
  o["encoding"] = picojson::value(encoding);
  lastStreamId++;

  PostAsyncSuccessReply(msg, o);
}

static bool RecursiveDeleteDirectory(const std::string& path) {
  DIR* dir = opendir(path.c_str());
  if (!dir)
    return false;
  struct dirent entry, *buffer;
  int fd = dirfd(dir);
  if (fd < 0)
    goto error;

  while (!readdir_r(dir, &entry, &buffer)) {
    struct stat st;

    if (!buffer)
      break;
    if (!strcmp(entry.d_name, ".") || !strcmp(entry.d_name, ".."))
      continue;
    if (fstatat(fd, entry.d_name, &st, 0) < 0)
      continue;

    if (S_ISDIR(st.st_mode)) {
      const std::string next_path = path + "/" + entry.d_name;
      if (!RecursiveDeleteDirectory(next_path))
        goto error;
    } else if (unlinkat(fd, entry.d_name, 0) < 0) {
      goto error;
    }
  }

  closedir(dir);
  return rmdir(path.c_str()) >= 0;

 error:
  closedir(dir);
  return false;
}

void FilesystemInstance::HandleFileDeleteDirectory(const picojson::value& msg) {
  if (!msg.contains("directoryPath")) {
    PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
    return;
  }

  bool recursive = msg.get("recursive").evaluate_as_boolean();
  std::string real_path = GetRealPath(msg.get("directoryPath").to_str());
  if (real_path.empty()) {
    PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
    return;
  }

  if (recursive) {
    if (!RecursiveDeleteDirectory(real_path)) {
      PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
      return;
    }
  } else if (rmdir(real_path.c_str()) < 0) {
    PostAsyncErrorReply(msg, IO_ERR);
    return;
  }

  PostAsyncSuccessReply(msg);
}

void FilesystemInstance::HandleFileDeleteFile(const picojson::value& msg) {
  if (!msg.contains("filePath")) {
    PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
    return;
  }

  std::string real_path = GetRealPath(msg.get("filePath").to_str());
  if (real_path.empty()) {
    PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
    return;
  }

  if (unlink(real_path.c_str()) < 0) {
    switch (errno) {
    case EACCES:
    case EBUSY:
    case EIO:
    case EPERM:
      PostAsyncErrorReply(msg, IO_ERR);
      break;
    case ENOENT:
      PostAsyncErrorReply(msg, NOT_FOUND_ERR);
      break;
    default:
      PostAsyncErrorReply(msg, UNKNOWN_ERR);
    }
  } else {
    PostAsyncSuccessReply(msg);
  }
}

void FilesystemInstance::HandleFileListFiles(const picojson::value& msg) {
  if (!msg.contains("fullPath")) {
    PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
    return;
  }

  std::string real_path = GetRealPath(msg.get("fullPath").to_str());
  if (real_path.empty()) {
    PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
    return;
  }

  DIR* directory = opendir(real_path.c_str());
  if (!directory) {
    PostAsyncErrorReply(msg, IO_ERR);
    return;
  }

  picojson::value::array a;

  struct dirent entry, *buffer;
  while (!readdir_r(directory, &entry, &buffer)) {
    if (!buffer)
      break;
    if (!strcmp(entry.d_name, ".") || !strcmp(entry.d_name, ".."))
      continue;

    a.push_back(picojson::value(JoinPath(msg.get("fullPath").to_str(),
                                         entry.d_name)));
  }

  closedir(directory);

  picojson::value v(a);
  PostAsyncSuccessReply(msg, v);
}


bool FilesystemInstance::CopyAndRenameSanityChecks(const picojson::value& msg,
      const std::string& from, const std::string& to, bool overwrite) {
  bool destination_file_exists = true;
  if (access(to.c_str(), F_OK) < 0) {
    if (errno == ENOENT) {
      destination_file_exists = false;
    } else {
      PostAsyncErrorReply(msg, IO_ERR);
      return false;
    }
  }

  unsigned found = to.find_last_of("/\\");
  struct stat destination_parent_st;
  if (stat(to.substr(0, found).c_str(), &destination_parent_st) < 0) {
    PostAsyncErrorReply(msg, IO_ERR);
    return false;
  }

  if (overwrite && !IsWritable(destination_parent_st)) {
    PostAsyncErrorReply(msg, IO_ERR);
    return false;
  }
  if (!overwrite && destination_file_exists) {
    PostAsyncErrorReply(msg, IO_ERR);
    return false;
  }

  if (access(from.c_str(), F_OK)) {
    PostAsyncErrorReply(msg, NOT_FOUND_ERR);
    return false;
  }

  return true;
}

namespace {

class PosixFile {
 private:
  int fd_;
  int mode_;
  std::string path_;
  bool unlink_when_done_;
 public:
  PosixFile(const std::string& path, int mode)
      : fd_(open(path.c_str(), mode, kDefaultFileMode))
      , mode_(mode)
      , path_(path)
      , unlink_when_done_(mode & O_CREAT) {}
  ~PosixFile();

  bool is_valid() { return fd_ >= 0; }

  void UnlinkWhenDone(bool setting) { unlink_when_done_ = setting; }

  ssize_t Read(char* buffer, size_t count);
  ssize_t Write(char* buffer, size_t count);
};

PosixFile::~PosixFile() {
  if (fd_ < 0)
    return;

  close(fd_);
  if (unlink_when_done_)
    unlink(path_.c_str());
}

ssize_t PosixFile::Read(char* buffer, size_t count) {
  if (fd_ < 0)
    return -1;

  while (true) {
    ssize_t read_bytes = read(fd_, buffer, count);
    if (read_bytes < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    return read_bytes;
  }
}

ssize_t PosixFile::Write(char* buffer, size_t count) {
  if (fd_ < 0)
    return -1;

  while (true) {
    ssize_t written_bytes = write(fd_, buffer, count);
    if (written_bytes < 0) {
      if (errno == EINTR)
        continue;
      return -1;
    }
    return written_bytes;
  }
}

}  // namespace

void FilesystemInstance::HandleFileCopyTo(const picojson::value& msg) {
  if (!msg.contains("originFilePath")) {
    PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
    return;
  }
  if (!msg.contains("destinationFilePath")) {
    PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
    return;
  }

  bool overwrite = msg.get("overwrite").evaluate_as_boolean();
  std::string real_origin_path =
     GetRealPath(msg.get("originFilePath").to_str());
  std::string real_destination_path =
     GetRealPath(msg.get("destinationFilePath").to_str());

  if (*real_destination_path.rbegin() == '/' ||
      *real_destination_path.rbegin() == '\\') {
    unsigned found = real_origin_path.find_last_of("/\\");
    real_destination_path.append(real_origin_path.substr(found + 1));
  }

  if (!CopyAndRenameSanityChecks(msg, real_origin_path, real_destination_path,
                                 overwrite))
    return;

  PosixFile origin(real_origin_path, O_RDONLY);
  if (!origin.is_valid()) {
    PostAsyncErrorReply(msg, IO_ERR);
    return;
  }

  PosixFile destination(real_destination_path, O_WRONLY | O_CREAT |
                                               (overwrite ? O_TRUNC : O_EXCL));
  if (!destination.is_valid()) {
    PostAsyncErrorReply(msg, IO_ERR);
    return;
  }

  while (true) {
    char buffer[512];
    ssize_t read_bytes = origin.Read(buffer, 512);
    if (!read_bytes)
      break;
    if (read_bytes < 0) {
      PostAsyncErrorReply(msg, IO_ERR);
      return;
    }

    if (destination.Write(buffer, read_bytes) < 0) {
      PostAsyncErrorReply(msg, IO_ERR);
      return;
    }
  }

  destination.UnlinkWhenDone(false);
  PostAsyncSuccessReply(msg);
}

void FilesystemInstance::HandleFileMoveTo(const picojson::value& msg) {
  if (!msg.contains("originFilePath")) {
    PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
    return;
  }
  if (!msg.contains("destinationFilePath")) {
    PostAsyncErrorReply(msg, INVALID_VALUES_ERR);
    return;
  }

  bool overwrite = msg.get("overwrite").evaluate_as_boolean();
  std::string real_origin_path =
     GetRealPath(msg.get("originFilePath").to_str());
  std::string real_destination_path =
     GetRealPath(msg.get("destinationFilePath").to_str());

  if (!CopyAndRenameSanityChecks(msg, real_origin_path, real_destination_path,
                                 overwrite))
    return;

  if (rename(real_origin_path.c_str(), real_destination_path.c_str()) < 0) {
    PostAsyncErrorReply(msg, IO_ERR);
    return;
  }

  PostAsyncSuccessReply(msg);
}

void FilesystemInstance::HandleSyncMessage(const char* message) {
  picojson::value v;

  std::string err;
  picojson::parse(v, message, message + strlen(message), &err);
  if (!err.empty()) {
    std::cout << "Ignoring sync message.\n";
    return;
  }

  std::string cmd = v.get("cmd").to_str();
  std::string reply;
  if (cmd == "FileSystemManagerGetMaxPathLength")
    HandleFileSystemManagerGetMaxPathLength(v, reply);
  else if (cmd == "FileStreamClose")
    HandleFileStreamClose(v, reply);
  else if (cmd == "FileStreamRead")
    HandleFileStreamRead(v, reply);
  else if (cmd == "FileStreamWrite")
    HandleFileStreamWrite(v, reply);
  else if (cmd == "FileCreateDirectory")
    HandleFileCreateDirectory(v, reply);
  else if (cmd == "FileCreateFile")
    HandleFileCreateFile(v, reply);
  else if (cmd == "FileGetURI")
    HandleFileGetURI(v, reply);
  else if (cmd == "FileResolve")
    HandleFileResolve(v, reply);
  else if (cmd == "FileStat")
    HandleFileStat(v, reply);
  else if (cmd == "FileStreamStat")
    HandleFileStreamStat(v, reply);
  else if (cmd == "FileStreamSetPosition")
    HandleFileStreamSetPosition(v, reply);
  else
    std::cout << "Ignoring unknown command: " << cmd;

  if (!reply.empty())
    SendSyncReply(reply.c_str());
}

void FilesystemInstance::HandleFileSystemManagerGetMaxPathLength(
      const picojson::value& msg, std::string& reply) {
  int max_path = pathconf("/", _PC_PATH_MAX);
  if (max_path < 0)
    max_path = PATH_MAX;

  picojson::value value(static_cast<double>(max_path));
  SetSyncSuccess(reply, value);
}

bool FilesystemInstance::IsKnownFileStream(const picojson::value& msg) {
  if (!msg.contains("streamID"))
    return false;
  unsigned int key = msg.get("streamID").get<double>();

  return fstream_map_.find(key) != fstream_map_.end();
}

std::fstream* FilesystemInstance::GetFileStream(unsigned int key) {
  FStreamMap::iterator it = fstream_map_.find(key);
  if (it == fstream_map_.end())
    return NULL;
  std::fstream* fs = it->second.second;

  if (fs->is_open())
    return fs;
  return NULL;
}

std::fstream* FilesystemInstance::GetFileStream(unsigned int key,
    std::ios_base::openmode mode) {
  FStreamMap::iterator it = fstream_map_.find(key);
  if (it == fstream_map_.end())
    return NULL;

  if ((it->second.first & mode) != mode)
    return NULL;

  std::fstream* fs = it->second.second;

  if (fs->is_open())
    return fs;
  return NULL;
}

void FilesystemInstance::SetSyncError(std::string& output,
      WebApiAPIErrors error_type) {
  picojson::value::object o;

  o["isError"] = picojson::value(true);
  o["errorCode"] = picojson::value(static_cast<double>(error_type));
  picojson::value v(o);
  output = v.serialize();
}

void FilesystemInstance::SetSyncSuccess(std::string& reply,
      std::string& output) {
  picojson::value::object o;

  o["isError"] = picojson::value(false);
  o["value"] = picojson::value(output);

  picojson::value v(o);
  reply = v.serialize();
}

void FilesystemInstance::SetSyncSuccess(std::string& reply) {
  picojson::value::object o;

  o["isError"] = picojson::value(false);

  picojson::value v(o);
  reply = v.serialize();
}

void FilesystemInstance::SetSyncSuccess(std::string& reply,
      picojson::value& output) {
  picojson::value::object o;

  o["isError"] = picojson::value(false);
  o["value"] = output;

  picojson::value v(o);
  reply = v.serialize();
}

void FilesystemInstance::HandleFileStreamClose(const picojson::value& msg,
      std::string& reply) {
  if (!msg.contains("streamID")) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }
  unsigned int key = msg.get("streamID").get<double>();

  FStreamMap::iterator it = fstream_map_.find(key);
  if (it != fstream_map_.end()) {
    std::fstream* fs = it->second.second;
    if (fs->is_open())
      fs->close();
    delete fs;
    fstream_map_.erase(it);
  }

  SetSyncSuccess(reply);
}

namespace {
namespace base64 {

static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmn" \
      "opqrstuvwxyz0123456789+/";

std::string ConvertTo(std::string input) {
  std::string encoded;
  size_t input_len = input.length();

  for (size_t i = 0; i < input_len;) {
    unsigned triple = input[i];
    i++;

    triple <<= 8;
    if (i < input_len)
      triple |= input[i];
    i++;

    triple <<= 8;
    if (i < input_len)
      triple |= input[i];
    i++;

    encoded.push_back(chars[(triple & 0xfc0000) >> 18]);
    encoded.push_back(chars[(triple & 0x3f000) >> 12]);
    encoded.push_back((i > input_len + 1) ? '=' : chars[(triple & 0xfc0) >> 6]);
    encoded.push_back((i > input_len) ? '=' : chars[triple & 0x3f]);
  }

  return encoded;
}

int DecodeOne(char c) {
  if (c < '0') {
    if (c == '+')
      return 62;
    if (c == '/')
      return 63;
    return -1;
  }
  if (c <= '9')
    return c - '0' + 52;
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  return -1;
}

std::string ConvertFrom(std::string input) {
  std::string decoded;
  int input_len = input.length();

  if (input_len % 4)
    return input;

  int i;
  int c = 0;
  int decoded_bits = 0;
  for (i = 0; i < input_len;) {
    c = input[i++];
    if (c == '=')
      break;
    if (c > 255)
      continue;
    int decoded_byte = DecodeOne(c);
    if (decoded_byte < 0)
      continue;

    decoded_bits |= decoded_byte;

    if (i % 4 == 0) {
      decoded.push_back(static_cast<char>(decoded_bits >> 16));
      decoded.push_back(static_cast<char>(decoded_bits >> 8));
      decoded.push_back(static_cast<char>(decoded_bits));
      decoded_bits = 0;
    } else {
      decoded_bits <<= 6;
    }
  }

  if (c == '=') {
    switch (input_len - i) {
    case 0:
      return input;
    case 1:
      decoded.push_back(static_cast<char>(decoded_bits >> 10));
      break;
    case 2:
      decoded.push_back(static_cast<char>(decoded_bits >> 16));
      decoded.push_back(static_cast<char>(decoded_bits >> 8));
      break;
    }
  }

  return decoded;
}

}  // namespace base64

std::string ConvertCharacterEncoding(const char* from_encoding,
                                     const char* to_encoding, char* buffer,
                                     size_t buffer_len) {
  iconv_t cd = iconv_open(from_encoding, to_encoding);

  char converted[kMaxSize];
  char *converted_buffer = converted;
  size_t converted_len = sizeof(converted) - 1;

  do {
    if (iconv(cd, &buffer, &buffer_len, &converted_buffer, &converted_len)
        == (size_t) -1) {
      iconv_close(cd);
      return "";
    }
  } while (buffer_len > 0 && converted_len > 0);
  *converted_buffer = 0;

  iconv_close(cd);

  return std::string(converted, converted_len);
}

}  // namespace

void FilesystemInstance::HandleFileStreamRead(const picojson::value& msg,
      std::string& reply) {
  if (!IsKnownFileStream(msg)) {
    SetSyncError(reply, IO_ERR);
    return;
  }
  unsigned int key = msg.get("streamID").get<double>();

  std::streamsize count;
  if (msg.contains("count"))
    count = msg.get("count").get<double>();
  else
    count = kMaxSize;

  std::fstream* fs = GetFileStream(key, std::ios_base::in);
  if (!fs) {
    SetSyncError(reply, IO_ERR);
    return;
  }

  std::streampos initial_pos = fs->tellg();
  char buffer[kMaxSize] = { 0 };
  fs->read(buffer, count);
  fs->clear();
  std::streampos bytes_read = fs->tellg() - initial_pos;

  if (fs->bad()) {
    fs->clear();
    SetSyncError(reply, IO_ERR);
    return;
  }

  if (msg.get("type").to_str() == "Bytes") {
    picojson::value::array a;

    for (int i = 0; i < bytes_read; i++) {
      if (+buffer[i] != 0)
        a.push_back(picojson::value(static_cast<double>(buffer[i])));
    }

    picojson::value v(a);
    SetSyncSuccess(reply, v);
    return;
  }

  std::string buffer_as_string;
  if (msg.get("encoding").to_str() == "ISO-8859-1")
    buffer_as_string = ConvertCharacterEncoding("ISO_8859-1", "UTF-8", buffer,
                                                bytes_read);
  else
    buffer_as_string = std::string(buffer, bytes_read);

  if (msg.get("type").to_str() == "Base64") {
    std::string base64_buffer = base64::ConvertTo(buffer_as_string);
    SetSyncSuccess(reply, base64_buffer);
    return;
  }

  SetSyncSuccess(reply, buffer_as_string);
}

void FilesystemInstance::HandleFileStreamWrite(const picojson::value& msg,
      std::string& reply) {
  if (!msg.contains("data")) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }

  if (!IsKnownFileStream(msg)) {
    SetSyncError(reply, IO_ERR);
    return;
  }
  unsigned int key = msg.get("streamID").get<double>();

  std::fstream* fs = GetFileStream(key, std::ios_base::out);
  if (!fs) {
    SetSyncError(reply, IO_ERR);
    return;
  }

  std::string buffer;
  if (msg.get("type").to_str() == "Bytes") {
    picojson::array a = msg.get("data").get<picojson::array>();
    for (picojson::array::iterator iter = a.begin(); iter != a.end(); ++iter)
      buffer.append<int>(1, (*iter).get<double>());
  } else if (msg.get("type").to_str() == "Base64") {
    buffer = base64::ConvertFrom(msg.get("data").to_str());
  } else {
    buffer = msg.get("data").to_str();
  }

  // FIXME(ricardotk): get default platform encoding mode and compare.
  if (msg.get("encoding").to_str() == "ISO-8859-1")
    buffer = ConvertCharacterEncoding("ISO_8859-1", "UTF-8",
                                                &buffer[0], buffer.length());

  if (!((*fs) << buffer)) {
    fs->clear();
    SetSyncError(reply, IO_ERR);
    return;
  }
  fs->flush();

  SetSyncSuccess(reply);
}

void FilesystemInstance::HandleFileCreateDirectory(const picojson::value& msg,
      std::string& reply) {
  if (!msg.contains("fullPath")) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }
  if (!msg.contains("relativeDirPath")) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }

  std::string full_path = JoinPath(msg.get("fullPath").to_str(),
                                   msg.get("relativeDirPath").to_str());
  if (full_path.empty()) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }

  std::string real_path = GetRealPath(full_path);
  if (real_path.empty()) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }

  if (mkdir(real_path.c_str(), kDefaultFileMode) < 0) {
    SetSyncError(reply, IO_ERR);
    return;
  }

  SetSyncSuccess(reply, full_path);
}

void FilesystemInstance::HandleFileCreateFile(const picojson::value& msg,
      std::string& reply) {
  if (!msg.contains("fullPath")) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }
  if (!msg.contains("relativeFilePath")) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }

  std::string full_path = JoinPath(msg.get("fullPath").to_str(),
                                   msg.get("relativeFilePath").to_str());
  if (full_path.empty()) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }

  std::string real_path = GetRealPath(full_path);
  if (real_path.empty()) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }

  int result = open(real_path.c_str(), O_CREAT | O_WRONLY | O_EXCL,
        kDefaultFileMode);
  if (result < 0) {
    SetSyncError(reply, IO_ERR);
    return;
  }

  close(result);
  SetSyncSuccess(reply, full_path);
}

void FilesystemInstance::HandleFileGetURI(const picojson::value& msg,
      std::string& reply) {
  if (!msg.contains("fullPath")) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }
  std::string full_path = msg.get("fullPath").to_str();

  std::string real_path = GetRealPath(full_path);
  if (real_path.empty()) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }

  char* real_path_c = realpath(real_path.c_str(), NULL);
  if (!real_path_c) {
    SetSyncError(reply, NOT_FOUND_ERR);
    return;
  }
  free(real_path_c);

  std::string uri_path = JoinPath("file:/", full_path);

  SetSyncSuccess(reply, uri_path);
}

void FilesystemInstance::HandleFileResolve(const picojson::value& msg,
      std::string& reply) {
  if (!msg.contains("fullPath")) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }
  if (!msg.contains("relativeFilePath")) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }

  std::string full_path = JoinPath(msg.get("fullPath").to_str(),
                                   msg.get("relativeFilePath").to_str());
  if (full_path.empty()) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }

  std::string real_path = GetRealPath(full_path);
  if (real_path.empty()) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }

  char* real_path_c = realpath(real_path.c_str(), NULL);
  if (!real_path_c) {
    SetSyncError(reply, NOT_FOUND_ERR);
    return;
  }
  free(real_path_c);

  SetSyncSuccess(reply, full_path);
}

void FilesystemInstance::HandleFileStat(const picojson::value& msg,
      std::string& reply) {
  if (!msg.contains("fullPath")) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }

  std::string real_path = GetRealPath(msg.get("fullPath").to_str());
  if (real_path.empty()) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }

  struct stat st;
  if (stat(real_path.c_str(), &st) < 0) {
    SetSyncError(reply, IO_ERR);
    return;
  }

  bool is_directory = !!S_ISDIR(st.st_mode);

  picojson::value::object o;
  o["size"] = picojson::value(static_cast<double>(st.st_size));
  o["modified"] = picojson::value(static_cast<double>(st.st_mtime));
  o["created"] = picojson::value(static_cast<double>(st.st_ctime));  // ?
  o["readOnly"] = picojson::value(!IsWritable(st));
  o["isFile"] = picojson::value(!!S_ISREG(st.st_mode));
  o["isDirectory"] = picojson::value(is_directory);
  if (is_directory)
    o["length"] = picojson::value(
        static_cast<double>(get_dir_entry_count(real_path.c_str())));


  picojson::value v(o);
  SetSyncSuccess(reply, v);
}

void FilesystemInstance::HandleFileStreamStat(const picojson::value& msg,
      std::string& reply) {
  if (!IsKnownFileStream(msg)) {
    SetSyncError(reply, IO_ERR);
    return;
  }
  unsigned int key = msg.get("streamID").get<double>();

  std::fstream* fs = GetFileStream(key);
  if (!fs) {
    SetSyncError(reply, IO_ERR);
    return;
  }

  std::streampos bytes_read = -1;
  if (!fs->eof()) {
    std::streampos initial_pos = fs->tellg();
    char buffer[kMaxSize] = { 0 };
    fs->read(buffer, kMaxSize);
    bytes_read = fs->tellg() - initial_pos;
    if (fs->bad()) {
      fs->clear();
      SetSyncError(reply, IO_ERR);
      return;
    }
    // Recover the position.
    fs->clear();
    fs->seekg(initial_pos);
    if (fs->bad()) {
      fs->clear();
      SetSyncError(reply, IO_ERR);
      return;
    }
  }

  picojson::value::object o;
  o["position"] = picojson::value(static_cast<double>(fs->tellg()));
  o["eof"] = picojson::value(fs->eof());
  o["bytesAvailable"] = picojson::value(static_cast<double>(bytes_read));

  picojson::value v(o);
  SetSyncSuccess(reply, v);
}

void FilesystemInstance::HandleFileStreamSetPosition(const picojson::value& msg,
      std::string& reply) {
  if (!msg.contains("position")) {
    SetSyncError(reply, INVALID_VALUES_ERR);
    return;
  }
  if (!IsKnownFileStream(msg)) {
    SetSyncError(reply, IO_ERR);
    return;
  }
  unsigned int key = msg.get("streamID").get<double>();

  std::fstream* fs = GetFileStream(key);
  if (!fs) {
    SetSyncError(reply, IO_ERR);
    return;
  }

  int position = msg.get("position").get<double>();
  fs->seekg(position);
  if (fs->bad()) {
    fs->clear();
    SetSyncError(reply, IO_ERR);
    return;
  }

  SetSyncSuccess(reply);
}

std::string FilesystemInstance::GetRealPath(const std::string& fullPath) {
  std::size_t pos = fullPath.find_first_of('/');
  std::string virtual_root = fullPath;

  if (pos != std::string::npos) {
    virtual_root = fullPath.substr(0, pos);
  }

  Storages::const_iterator it = storages_.find(virtual_root);

  if (it == storages_.end())
    return std::string();

  if (pos != std::string::npos)
    return it->second.GetFullPath() + fullPath.substr(pos);

  return it->second.GetFullPath();
}

void FilesystemInstance::AddInternalStorage(
    const std::string& label, const std::string& path) {
  if (makePath(path))
    storages_.insert(SorageLabelPair(label,
                                     Storage(-1,
                                             Storage::STORAGE_TYPE_INTERNAL,
                                             Storage::STORAGE_STATE_MOUNTED,
                                             path)));
}

void FilesystemInstance::AddStorage(int id,
                                   storage_type_e type,
                                   storage_state_e state,
                                   const std::string& path) {
  std::string label;
  if (type == STORAGE_TYPE_INTERNAL)
    label = kInternalStorage + std::to_string(id);
  else if (type == STORAGE_TYPE_EXTERNAL)
    label = kRemovableStorage + std::to_string(id);

  storages_.insert(SorageLabelPair(label,
                                   Storage(id,
                                           type,
                                           state,
                                           path)));
  if (std::find(watched_storages_.begin(),
                watched_storages_.end(), id) != watched_storages_.end()) {
    watched_storages_.push_back(id);
    storage_set_state_changed_cb(id, OnStorageStateChanged, this);
  }
}

void FilesystemInstance::NotifyStorageStateChanged(int id,
                                                  storage_state_e state) {
  for (Storages::iterator it = storages_.begin(); it != storages_.end(); ++it) {
    if (it->second.GetId() == id) {
      it->second.SetState(state);
      picojson::object reply;
      reply["storage"] = picojson::value(it->second.toJSON(it->first));
      reply["cmd"] = picojson::value("storageChanged");
      picojson::value value(reply);
      PostMessage(value.serialize().c_str());
      break;
    }
  }
}

bool FilesystemInstance::OnStorageDeviceSupported(
    int id, storage_type_e type, storage_state_e state,
    const char *path, void* user_data) {
  reinterpret_cast<FilesystemInstance*>(user_data)->AddStorage(
      id, type, state, path);
  return true;
}

void FilesystemInstance::OnStorageStateChanged(
    int id, storage_state_e state, void* user_data) {
  reinterpret_cast<FilesystemInstance*>(user_data)->NotifyStorageStateChanged(
      id, state);
}

FilesystemInstance::Storage::Storage(
    int id, int type, int state, const std::string& fullpath)
  : id_(id),
    type_(type),
    state_(state),
    fullpath_(fullpath) { }

picojson::object FilesystemInstance::Storage::toJSON(
    const std::string& label) const {
  picojson::object storage_object;
  storage_object["label"] = picojson::value(label);
  storage_object["type"] = picojson::value(type());
  storage_object["state"] = picojson::value(state());
  return storage_object;
}

std::string FilesystemInstance::Storage::type() const {
  return (type_ == Storage::STORAGE_TYPE_INTERNAL) ? kStorageTypeInternal :
      kStorageTypeExternal;
}

std::string FilesystemInstance::Storage::state() const {
  switch (state_) {
  case Storage::STORAGE_STATE_MOUNTED:
  case Storage::STORAGE_STATE_MOUNTED_READONLY:
    return kStorageStateMounted;
  case Storage::STORAGE_STATE_REMOVED:
    return kStorageStateRemoved;
  case Storage::STORAGE_STATE_UNMOUNTABLE:
    return kStorageStateUnmountable;
  default:
    assert(!"Not reached");
  }
  return std::string();
}
