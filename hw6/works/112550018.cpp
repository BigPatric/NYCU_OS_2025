#define FUSE_USE_VERSION 30
#include <fuse.h>
#include <string.h>
#include <unistd.h>
int my_readdir(const char *path, void *buffer, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi) { 
    DIR *dp;
    struct dirent *de;

    dp = opendir(path);
    if (dp == NULL)
        return 0;
    while ((de = readdir(dp)) != NULL) {
        if(filler(buffer, de->d_name, NULL, 0)){
            break;
        }
    }
    closedir(dp);
    return 0;
}
int my_getattr(const char *path, struct stat *st) { 
    int res = lstat(path, st);
    if (res == -1) {
        return -1;
    }
    return 0;
    
 }
int my_read(const char *path, char *buffer, size_t size, off_t offset, struct fuse_file_info *fi) { 

    int fd;
    int res;

    fd = open(path, O_RDONLY);
    if (fd == -1)
        return -1;

    res = pread(fd, buffer, size, offset);
    if (res == -1)
        res = -1;
    close(fd);
    return res;
}
int readlink(const char *path, void *buffer, size_t size) {
    int res = readlink(path, (char *)buffer, size - 1);
    return 0;
}
static struct fuse_operations op;
int main(int argc, char *argv[])
{
memset(&op, 0, sizeof(op));
op.getattr = my_getattr;
op.readdir = my_readdir;
op.read = my_read;
op.readlink = my_readlink;
return fuse_main(argc, argv, &op, NULL);
}