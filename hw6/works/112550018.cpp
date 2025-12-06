#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <limits.h>
#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <set>

using namespace std;

class Tar_Header {
 public:
  char filename[100];
  char mode[8];
  char uid[8];
  char gid[8];
  char fileSize[12];
  char lastModification[12];
  char checksum[8];
  char linkFlag;
  char linkedFileName[100];
  char magic[6];
  char ustarVersion[2];
  char ownerUserName[32];
  char ownerGroupName[32];
  char deviceMajorNumber[8];
  char deviceMinorNumber[8];
  char filenamePrefix[155];
  char padding[12];

  static uint64_t octal2decimal(const char *data, size_t size) {
    uint64_t n = 0;
    for (size_t i = 0; i < size && isdigit((unsigned char)data[i]); i++) {
      n = (n << 3) | (unsigned int) (data[i] - '0');
    }
    return n;
  }

  size_t getFileSize() const {
    return octal2decimal(fileSize, 12);
  }

  mode_t getMode() const {
    return octal2decimal(mode, 8);
  }

  short getUid() const {
    return octal2decimal(uid, 8);
  }

  short getGid() const {
    return octal2decimal(gid, 8);
  }

  time_t getMtime() const {
    return octal2decimal(lastModification, 12);
  }
};

// in-memory structures
static map<string, set<string>> file_directory; // key: "/dir/" -> set{ "entry" }
static map<string, struct stat *> file_attribute; // key: "/path" -> stat*
static map<string, char *> file_content; // key: "/path" -> content (for files) or link target (for symlink)

static struct fuse_operations op;

// helpers
static string makeFullName(const Tar_Header &h) {
  string prefix(h.filenamePrefix, strnlen(h.filenamePrefix, sizeof(h.filenamePrefix)));
  string name(h.filename, strnlen(h.filename, sizeof(h.filename)));
  if (!prefix.empty()) {
    if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');
    return prefix + name;
  } else {
    return name;
  }
}

static void add_entry_to_directory_maps(const string &fullpath, bool is_dir) {
  // Ensure fullpath starts with '/'
  string path = fullpath;
  if (path.size() == 0 || path[0] != '/') path.insert(0, "/");

  // For splitting, remove trailing '/' (but keep "/" itself)
  string path_for_split = path;
  if (path_for_split.size() > 1 && path_for_split.back() == '/') path_for_split.pop_back();

  // find parent and name
  size_t pos = path_for_split.find_last_of('/');
  string parent;
  string name;
  if (pos == string::npos || pos == 0) {
    parent = "/";
    name = path_for_split.substr(pos + 1); // works for pos==0 -> substr(1)
  } else {
    parent = path_for_split.substr(0, pos + 1); // include trailing '/'
    name = path_for_split.substr(pos + 1);
  }

  if (parent.empty()) parent = "/";
  if (parent.back() != '/') parent.push_back('/');

  if (is_dir) {
    // ensure directory key exists (with trailing '/')
    string dirkey = path;
    if (dirkey.back() != '/') dirkey.push_back('/');
    // add entry in parent
    if (!name.empty()) file_directory[parent].insert(name);
    // ensure directory map exists
    if (file_directory.count(dirkey) == 0) file_directory[dirkey] = set<string>();
  } else {
    if (!name.empty()) file_directory[parent].insert(name);
  }
}

// FUSE callbacks using the in-memory maps
int my_getattr(const char *path, struct stat *st) {
  memset(st, 0, sizeof(struct stat));
  string p(path);
  if (p == "/") {
    st->st_mode = S_IFDIR | 0444;
    return 0;
  }
  // file_attribute keys should have leading '/'
  if (file_attribute.count(p) == 1) {
    struct stat *src = file_attribute[p];
    st->st_mode = src->st_mode;
    st->st_uid = src->st_uid;
    st->st_gid = src->st_gid;
    st->st_size = src->st_size;
    st->st_mtime = src->st_mtime;
    return 0;
  }
  // maybe path refers to a directory without trailing slash, check with trailing '/'
  string pdir = p;
  if (pdir.back() != '/') pdir.push_back('/');
  if (file_attribute.count(pdir) == 1) {
    struct stat *src = file_attribute[pdir];
    st->st_mode = src->st_mode;
    st->st_uid = src->st_uid;
    st->st_gid = src->st_gid;
    st->st_size = src->st_size;
    st->st_mtime = src->st_mtime;
    return 0;
  }
  return -ENOENT;
}

int my_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) {
  (void) offset;
  (void) fi;

  string p(path);
  if (p.back() != '/') p.push_back('/');
  // always present . and ..
  filler(buffer, ".", NULL, 0);
  filler(buffer, "..", NULL, 0);

  if (file_directory.count(p) == 0) {
    return -ENOENT;
  }
  for (const string &entry : file_directory[p]) {
    filler(buffer, entry.c_str(), NULL, 0);
  }
  return 0;
}

int my_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) {
  string p(path);
  if (file_content.count(p) == 0) return 0; // not a file or empty file
  struct stat *st = file_attribute[p];
  if (!st) return -ENOENT;
  size_t fsize = st->st_size;
  if (offset >= (off_t)fsize) return 0;
  size_t toread = size;
  if (offset + (off_t)size > (off_t)fsize) toread = fsize - offset;
  memcpy(buffer, file_content[p] + offset, toread);
  return (int)toread;
}

int my_readlink(const char *path, char *buffer, size_t size) {
  string p(path);
  if (file_content.count(p) == 0) return -ENOENT;
  // treat stored content as null-terminated link target
  char *target = file_content[p];
  size_t len = strlen(target);
  if (len >= size) len = size - 1;
  memcpy(buffer, target, len);
  buffer[len] = '\0';
  return 0;
}

int main(int argc, char *argv[]) {
  // use test.tar
  const char *tarpath = "test.tar";

  ifstream file(tarpath, ios::in | ios::binary);
  if (!file.is_open()) {
    fprintf(stderr, "cannot open tar file: %s\n", tarpath);
    return 1;
  }

  // read loop
  while (true) {
    Tar_Header header;
    file.read((char *)&header, 512);
    if (!file) break;

    // check all-zero header => end
    bool all_zero = true;
    for (size_t i = 0; i < 512; ++i) {
      if (((unsigned char *)&header)[i] != 0) { all_zero = false; break; }
    }
    if (all_zero) break;

    string name = makeFullName(header);
    if (name.empty()) continue;
    // ensure leading '/'
    if (name[0] != '/') name.insert(0, "/");

    size_t fsize = header.getFileSize();
    bool is_dir = (header.linkFlag == '5') || (!name.empty() && name.back() == '/');
    bool is_symlink = (header.linkFlag == '2');

    // For directories, size may be zero; for symlink the linkedFileName stores target
    char *content = nullptr;
    if (is_symlink) {
      // use linkedFileName as target
      string target(header.linkedFileName, strnlen(header.linkedFileName, sizeof(header.linkedFileName)));
      content = new char[target.size() + 1];
      memcpy(content, target.c_str(), target.size() + 1);
    } else if (!is_dir) {
      content = new char[fsize + 1];
      if (fsize > 0) file.read(content, fsize);
      content[fsize] = '\0';
    }

    // construct stat
    struct stat *st = new struct stat;
    memset(st, 0, sizeof(struct stat));
    mode_t mode = header.getMode() & 0777;
    if (is_dir) {
      st->st_mode = S_IFDIR | mode;
      st->st_nlink = 2;
    } else if (is_symlink) {
      st->st_mode = S_IFLNK | mode;
      st->st_size = fsize;
      st->st_nlink = 1;
    } else {
      st->st_mode = S_IFREG | mode;
      st->st_size = fsize;
      st->st_nlink = 1;
    }
    st->st_uid = header.getUid();
    st->st_gid = header.getGid();
    st->st_mtime = header.getMtime();

    // Normalize path for directory keys
    string key = name;
    if (is_dir && key.back() != '/') key.push_back('/');

    // free existing if present
    if (file_attribute.count(name)) {
      delete file_attribute[name];
      file_attribute.erase(name);
    }
    if (file_content.count(name)) {
      delete[] file_content[name];
      file_content.erase(name);
    }

    file_attribute[name] = st;
    if (content) file_content[name] = content;

    // update directory map
    add_entry_to_directory_maps(name, is_dir);

    // skip padding for regular file data
    if (!is_symlink && !is_dir) {
      size_t pad = (512 - (fsize % 512)) % 512;
      if (pad > 0) file.ignore(pad);
    }
  }

  file.close();

  if (file_directory.count("/") == 0) file_directory["/"] = set<string>();

  memset(&op, 0, sizeof(op));
  op.getattr = my_getattr;
  op.readdir = my_readdir;
  op.read = my_read;
  op.readlink = (int (*)(const char *, char *, size_t))my_readlink;

  return fuse_main(argc, argv, &op, NULL);
}
