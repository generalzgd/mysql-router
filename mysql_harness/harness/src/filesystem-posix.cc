/*
  Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "filesystem.h"

#include <cassert>
#include <sstream>
#include <stdexcept>

#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if !defined(_POSIX_C_SOURCE) || _POSIX_C_SOURCE < 200112L
# error "This file expects POSIX.1-2001 or later"
#endif

#if defined(__sun) && defined(__SVR4) // Solaris
  #include <limits.h> // PATH_MAX; maybe <climits> would be ok too,
#endif                //           had no easy way of checking

using std::ostringstream;
using std::runtime_error;
using std::string;

namespace {
  const std::string dirsep("/");
  const std::string extsep(".");
}

namespace mysql_harness {

////////////////////////////////////////////////////////////////
// class Path members and free functions

const char * const Path::directory_separator = "/";
const char * const Path::root_directory = "/";

Path::FileType Path::type(bool refresh) const {
  validate_non_empty_path();
  if (type_ == FileType::TYPE_UNKNOWN || refresh) {
    struct stat stat_buf;
    if (stat(c_str(), &stat_buf) == -1) {
      if (errno == ENOENT || errno == ENOTDIR)
        type_ = FileType::FILE_NOT_FOUND;
      else
        type_ = FileType::STATUS_ERROR;
    } else {
      switch (stat_buf.st_mode & S_IFMT) {
      case S_IFDIR:
        type_ = FileType::DIRECTORY_FILE;
        break;
      case S_IFBLK:
        type_ = FileType::BLOCK_FILE;
        break;
      case S_IFCHR:
        type_ = FileType::CHARACTER_FILE;
        break;
      case S_IFIFO:
        type_ = FileType::FIFO_FILE;
        break;
      case S_IFLNK:
        type_ = FileType::SYMLINK_FILE;
        break;
      case S_IFREG:
        type_ = FileType::REGULAR_FILE;
        break;
      case S_IFSOCK:
        type_ = FileType::SOCKET_FILE;
        break;
      default:
        type_ = FileType::TYPE_UNKNOWN;
        break;
      }
    }
  }
  return type_;
}

////////////////////////////////////////////////////////////////
// Directory::DirectoryIterator

class Directory::DirectoryIterator::State {
 public:
  State();
  State(const Path& path, const string& pattern);
  ~State();

  bool eof() const {
    return result_ == nullptr;
  }

  void fill_result();

  template <typename IteratorType>
  static bool equal(const IteratorType& lhs, const IteratorType& rhs) {
    assert(lhs != nullptr && rhs != nullptr);

    // If either one is null (end iterators), they are equal if both
    // are end iterators.
    if (lhs->result_ == nullptr || rhs->result_ == nullptr)
      return lhs->result_ == rhs->result_;

    // Otherwise they are not equal (this is an input iterator and we
    // should not compare entries received through different
    // iterations.
    return false;
  }

  DIR *dirp_;
  struct dirent entry_;
  const string pattern_;
  struct dirent *result_;
};


Directory::DirectoryIterator::State::State()
  : dirp_(nullptr), pattern_(""), result_(nullptr) {}


Directory::DirectoryIterator::State::State(const Path& path,
                                           const string& pattern)
    : dirp_(opendir(path.c_str())), pattern_(pattern), result_(&entry_) {
  if (dirp_ == nullptr) {
    ostringstream buffer;
    char buf[256];
    if (strerror_r(errno, buf, sizeof(buf)) != 0)
      buffer << "Failed to open path " << path << " - " << errno;
    else
      buffer << "Failed to open path " << path << " - " << buf;
    throw runtime_error(buffer.str());
  }

  fill_result();
}


Directory::DirectoryIterator::State::~State() {
  // There is no guarantee that calling closedir() with NULL will
  // work. For example, BSD systems do not always support this.
  if (dirp_ != nullptr)
    closedir(dirp_);
}


void Directory::DirectoryIterator::State::fill_result() {
  // This is similar to scandir(2), but we do not use scandir(2) since
  // we want to be thread-safe.

  // If we have reached the end, filling do not have any effect.
  if (result_ == nullptr)
    return;

  while (true) {
    if (int error = readdir_r(dirp_, &entry_, &result_)) {
      ostringstream buffer;
      char msg[256];
      if (strerror_r(error, msg, sizeof(msg)))
        buffer << "strerror_r failed: " << errno;
      else
        buffer << "Failed to read directory entry - " << msg;
      throw std::runtime_error(buffer.str());
    }

    // If there are no more entries, we're done.
    if (result_ == nullptr)
      break;

    // Skip current directory and parent directory.
    if (strcmp(result_->d_name, ".") == 0 ||
        strcmp(result_->d_name, "..") == 0)
      continue;

    // If no pattern is given, we're done.
    if (pattern_.size() == 0)
      break;

    // Skip any entries that do not match the pattern
    int error = fnmatch(pattern_.c_str(), result_->d_name, FNM_PATHNAME);
    if (error == FNM_NOMATCH) {
      continue;
    } else if (error == 0) {
      break;
    } else {
      ostringstream buffer;
      char msg[256];
      if (strerror_r(error, msg, sizeof(msg)))
        buffer << "strerror_r failed: " << errno;
      else
        buffer << "Match failed - " << msg;
      throw std::runtime_error(buffer.str());
    }
  }
}


////////////////////////////////////////////////////////////////
// Directory::DirectoryIterator

// These definition of the default constructor and destructor need to
// be here since the automatically generated default
// constructor/destructor uses the definition of the class 'State',
// which is not available when the header file is read.
Directory::DirectoryIterator::~DirectoryIterator() = default;
Directory::DirectoryIterator::DirectoryIterator(
    DirectoryIterator&&) = default;
Directory::DirectoryIterator::DirectoryIterator(
    const DirectoryIterator&) = default;

Directory::DirectoryIterator::DirectoryIterator()
  : path_("*END*"), state_(std::make_shared<State>()) {}


Directory::DirectoryIterator::DirectoryIterator(const Path& path,
                              const string& pattern)
  : path_(path), state_(std::make_shared<State>(path, pattern)) {}


Directory::DirectoryIterator& Directory::DirectoryIterator::operator++() {
  assert(state_ != nullptr);
  state_->fill_result();
  return *this;
}


Path Directory::DirectoryIterator::operator*() const {
  assert(state_ != nullptr && state_->result_ != nullptr);
  return path_.join(state_->result_->d_name);
}

bool
Directory::DirectoryIterator::operator!=(
    const Directory::DirectoryIterator& rhs) const {
  return !State::equal(state_, rhs.state_);
}

Path
Path::make_path(const Path& dir,
                const std::string& base,
                const std::string& ext) {
  return dir.join(base + extsep + ext);
}

Path Path::real_path() const {
  validate_non_empty_path();
  char buf[PATH_MAX];
  if (realpath(c_str(), buf))
    return Path(buf);
  else
    return Path();
}

} // namespace mysql_harness
