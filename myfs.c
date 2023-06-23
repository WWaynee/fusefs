/**
 * VERSION 1.0
 * 只单纯按照文件大小决定存放位置。
 * 测试：gcc myfs.c -o myfs `pkg-config fuse --cflags --libs`
 * 运行：./myfs -f point
 * 
 * 问题：
 * (1) getattr的res是0但是会提示没有目录
 * (2) read传进来的size一直是4096
 * (3) truncate size大于4096，文件搬运到HDD但是不见踪影（？
 * (4) echo写入文件能成功但是会报错，涉及函数：open truncate getattr write
 * (5) 单纯rename没问题
 * 
 * VERSION 1.1
 * (1) cd某个目录没成功，getattr函数没问题，access函数有问题(已解决：路径参数传错)
 * (2) cat文件没内容输出，涉及函数：getattr、open、read(已解决：read和write函数应该返回res而不是返回0)
 * (3) echo append写入报错，但是文件内容没问题：input/output error，涉及getattr、open、write(已解决：没有用到参数记得(void)哦)
 * (4) SSD文件搬运到HDD后原目录ls看不到文件名了，但是可以echo写入，也可以stat读到文件属性，truncate会提示文件路径不存在
 * （这个问题是因为：文件搬运去HDD后在SSD的原目录看不到，不知道已经搬出去了。我觉得需要给HDD文件在原目录的SSD设置一个xattr）
 * 
 * VERSION 1.2
 * (1) 增加xattr保存HDD文件的元数据，增加symlink（命名为搬运到HDD的文件在SSD的原文件名），已知对symlink的echo会作用到原文件上
 * (2) 访问文件的方式：SSD文件（直接路径访问），HDD文件（SSD symlink -> HDD路径），访问HDD文件的元数据（SSD xattr）
 *      举个例子：文件/a/b/abc在SSD上，所以全路径是/home/SSD/a/b/abc
 *      搬运到HDD后，路径变成/home/HDD/a/b/abc，在SSD的symlink是/home/SSD/a/b/abc，xattr是/home/SSD/a/b/.xattr_abc
 * (3) 存疑点，xattr文件不创建直接open会不会有问题：truncate和write函数中的处理
 * 
 * VERSION 1.3
 * (1) 对symlink的echo操作没有作用到源文件，直接把symlink变成普通文件了，但是readlink还是能拿到源文件路径
 * (2) echo操作超出阈值出现bash: echo: write error: No such file or directory报错，文件也没迁移到HDD里（没看到），之后再ls -la还会出现软链接指向HDD路径
 * (3) ls -la会显示指向hdd的symlink
 * (4) 上述问题怎么多编译几次就没了
 * (5) 单独创建symlink的时候需要输入target的完整路径（原函数传递进来的参数没解析
*/
#define FUSE_USE_VERSION 26
#define _XOPEN_SOURCE 700

#include <fuse.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>

char SSDPATH[256] = "/home/ssd";
char HDDPATH[256] = "/home/hdd";
size_t THRESH = 512;

/**
 * SSDPATH + path 拼接成ssdpath
*/
void get_ssd_path(const char *path, char *ssd_path) {
    strcpy(ssd_path, SSDPATH);
    strcat(ssd_path, path);
}

/**
 * HDDPATH + path 拼接成hhdpath
*/
void get_hdd_path(const char *path, char *hdd_path) {
    strcpy(hdd_path, HDDPATH);
    strcat(hdd_path, path);
}

void get_xattr_path(const char *path, char *xattr_path) {
    strcpy(xattr_path, SSDPATH);
    strcat(xattr_path, path);
    strcpy(strrchr(xattr_path, '/') + 1, ".xattr_");
    strcat(xattr_path, strrchr(path, '/') + 1);
}

void get_xattr_with_ssdpath(const char *ssd_path, char *xattr_path) {
    strcpy(xattr_path, ssd_path);
    strcpy(strrchr(xattr_path, '/') + 1, ".xattr_");
    strcat(xattr_path, strrchr(ssd_path, '/') + 1);
}

int xattr_exist(const char *path) {
    int res;
    char xattr_path[256];
    get_xattr_path(path, xattr_path);
    struct stat st;
    res = lstat(xattr_path, &st);
    if (res == 0) return 1;
    return 0;
}

/**
 * readlink(ssdpath) = hddpath
*/
int get_hddpath_from_ssdpath(char *ssd_path, char *hdd_path) {
    struct stat st;
    int res;
    res = lstat(ssd_path, &st);
    if (res == -1) return -errno;
    if (!S_ISLNK(st.st_mode)) return -errno;
    res = readlink(ssd_path, hdd_path, 255);
    if (res == -1) return -errno;
    hdd_path[res] = 0;
    return 0;
}

/**
 * 必须放在symlink创建之后，xattr_path必须是空的
*/
void update_create_symlink_with_ssdpath(char *ssd_path, char *xattr_path) {
    char hdd_path[256];
    // 通过SSD的symlink获取hddpath
    get_hddpath_from_ssdpath(ssd_path, hdd_path);
    // 获取xattr文件
    get_xattr_with_ssdpath(ssd_path, xattr_path);
    struct stat st;
    lstat(ssd_path, &st);
    assert(S_ISLNK(st.st_mode));
    // 打开xattr文件并写入HDD文件的stat
    lstat(hdd_path, &st);
    // O_CREAT：如果文件不存在就创建为常规文件，所以创建xattr文件这里是没问题的
    int fd = open(xattr_path, O_WRONLY|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH);
    write(fd, &st, sizeof(st));
    close(fd);
}

int my_getattr(const char *path, struct stat *st) {
    char ssd_path[256];
    get_ssd_path(path, ssd_path);
    int res;
    // 如果文件是symlink读取的是symlink本身的属性
    res = lstat(ssd_path, st);
    // symlink文件名是同名的，如果不存在说明在HDD上也不存在
    if (res == -1) return -errno;
    // 如果当前文件是symlink类型且存在xattr文件，说明文件在HDD上
    if (S_ISLNK(st->st_mode) && xattr_exist(path)) {
        int fd;
        char xattr_path[256];
        get_xattr_path(path, xattr_path);
        fd = open(xattr_path, O_RDONLY);
        if (fd == -1) return -errno;
        // 将xattr的内容（即对应HDD文件的属性）读到st里面
        res = read(fd, st, sizeof(struct stat));
        if (res != sizeof(struct stat)) return -errno;
        printf("my_getattr: symlink %s\n", xattr_path);
        return 0;
    }
    printf("my_getattr: %s\n", ssd_path);
    return res;
}

int my_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
			struct fuse_file_info *fi) {
    DIR *dir;
    struct dirent *de;
    char ssd_path[256];
    get_ssd_path(path, ssd_path);
    dir = opendir(ssd_path);
    if (dir == NULL)
        return -errno;
    while ((de = readdir(dir)) != NULL) {
        // xattr文件应该跳过
        if (strncmp(de->d_name, ".xattr_", 7) == 0) continue;
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = de->d_ino;
        st.st_mode = de->d_type;
        filler(buf, de->d_name, &st, 0);
    }
    closedir(dir);
    printf("my_readdir: %s\n", ssd_path);
    return 0;
}

int my_access(const char *path, int mode) {
    char ssd_path[256];
    get_ssd_path(path, ssd_path);
    // 如果文件是在HDD上
    if (xattr_exist(path)) {
        // 获取SSD文件路径并访问
        char hdd_path[256];
        get_hddpath_from_ssdpath(ssd_path, hdd_path);
        int res = access(hdd_path, mode);
        if (res == -1) return -errno;
        printf("my_access: %s\n", hdd_path);
        return 0;
    }
    int res = access(ssd_path, mode);
    if (res == -1) return -errno;
    printf("my_access: %s\n", ssd_path);
    return 0;
}

/**
 * 读取SSD上的symlink
*/
int my_readlink(const char *path, char *buf, size_t size) {
    char ssd_path[256];
    get_ssd_path(path, ssd_path);
    int res;
    res = readlink(ssd_path, buf, size - 1);
    if (res == -1) return -errno;
    buf[res] = 0;
    printf("my_readlink: %s\n", ssd_path);
    return 0;
}

/**
 * 创建symlink，如果link_target是HDD文件，直接link其在SSD上的symlink即可
*/
int my_symlink(const char *target, const char *linkname) {
    char target_path[256], link_path[256];
    get_ssd_path(target, target_path);
    get_ssd_path(linkname, link_path);
    int res;
    res = symlink(target_path, link_path);
    if (res == -1) return -errno;
    printf("my_symlink: %s %s\n", target_path, link_path);
    return 0;
}

/**
 * 删除文件
*/
int my_unlink(const char *path) {
    char ssd_path[256];
    get_ssd_path(path, ssd_path);
    int res;
    // 如果文件在HDD上，那HDD文件、symlink和xattr也要删除
    if (xattr_exist(path)) {
        char hdd_path[256], xattr_path[256];
        // 应该通过symlink获取hdd路径，因为rename时不会修改HDD路径，所以文件路径不一定是HDDPATH + 文件名
        get_hddpath_from_ssdpath(ssd_path, hdd_path);
        get_xattr_path(path, xattr_path);
        res = unlink(hdd_path); // 删除源文件
        res = unlink(xattr_path);   // 删除SSD上的元数据
        res = unlink(ssd_path); // 删除SSD上的symlink
        if (res == -1) return -errno;
        printf("my_unlink: %s\n", hdd_path);
    } else {
        res = unlink(ssd_path); // 删除SSD文件
        if (res == -1) return -errno;
        printf("my_unlink: %s\n", ssd_path);
    }
    return 0;
}

int my_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    // 创建文件都在SSD进行
    char ssd_path[256];
    get_ssd_path(path, ssd_path);
    int res;
    res = creat(ssd_path, mode);
    if (res = -1) return -errno;
    close(res);
    printf("my_create: %s\n", ssd_path);
    return 0;
}

int my_mknod(const char *path, mode_t mode, dev_t dev) {
    char ssd_path[256];
    get_ssd_path(path, ssd_path);
    int res;
    if (S_ISFIFO(mode)) {
        res = mkfifo(ssd_path, mode);
    } else {
        res = mknod(ssd_path, mode, dev);
    }
    if (res = -1) return -errno;
    printf("my_mknod: %s\n", ssd_path);
    return 0;
}

int my_mkdir(const char *path, mode_t mode) {
    char ssd_path[256];
    get_ssd_path(path, ssd_path);
    int res;
    res = mkdir(ssd_path, mode);
    if (res == -1) return -errno;
    printf("my_mkdir: %s\n", ssd_path);
    return 0;
}

int my_rmdir(const char *path) {
    char ssd_path[256];
    get_ssd_path(path, ssd_path);
    int res;
    res = rmdir(ssd_path);
    if (res == -1) return -errno;
    printf("my_rmdir: %s\n", ssd_path);
    return 0;
}

/**
 * rename SSD文件：直接进行
 * rename HDD文件：symlink rename + xattr rename + 源文件不动（因为有symlink保存路径）
*/
int my_rename(const char *from, const char *to) {
    char ssd_from[256], ssd_to[256];
    get_ssd_path(from, ssd_from);
    get_ssd_path(to, ssd_to);
    int res;
    res = rename(ssd_from, ssd_to);
    if (res == -1) return -errno;
    if (xattr_exist(from)) {
        char xattr_from[256], xattr_to[256];
        get_xattr_path(from, xattr_from);
        get_xattr_path(to, xattr_to);
        res = rename(xattr_from, xattr_to);
    }
    if (res == -1) return -errno;
    printf("my_rename: %s to %s\n", ssd_from, ssd_to);
    return 0;
}

int my_chmod(const char *path, mode_t mode) {
    char ssd_path[256];
    get_ssd_path(path, ssd_path);
    // 如果是HDD文件
    if (xattr_exist(path)) {
        char hdd_path[256], xattr_path[256];
        get_hddpath_from_ssdpath(ssd_path, hdd_path);
        int res;
        res = chmod(hdd_path, mode);
        if (res == -1) return -errno;
        // 由于修改了文件属性，所以需要更新xattr元数据
        struct stat st;
        lstat(hdd_path, &st);
        get_xattr_path(path, xattr_path);
        int fd;
        fd = open(xattr_path, O_WRONLY);
        if (fd < 0) return -errno;
        // 将stat写到xattr文件里
        res = write(fd, &st, sizeof(st));
        if (res != sizeof(st)) {
            close(fd);
            return -errno;
        }
        close(fd);
        printf("my_chmod: %s %d\n", hdd_path, mode);
        return 0;
    }
    if (chmod(ssd_path, mode) == -1) return -errno;
    printf("my_chmod: %s %d\n", ssd_path, mode);
    return 0;
}

int my_chown(const char *path, uid_t uid, gid_t gid) {
    char ssd_path[256];
    get_ssd_path(path, ssd_path);
    int res;
    res = lchown(ssd_path, uid, gid);
    if (res == -1) return -errno;
    printf("my_chown: %s %d %d\n", ssd_path, uid, gid);
    return 0;
}

int my_truncate(const char *path, off_t size) {
    char ssd_path[256], hdd_path[256], xattr_path[256];
    get_ssd_path(path, ssd_path);
    get_xattr_with_ssdpath(ssd_path, xattr_path);
    if (size >= THRESH) {
        // 文件在HDD上
        if (xattr_exist(path)) {
            get_hddpath_from_ssdpath(ssd_path, hdd_path);
            truncate(hdd_path, size);
        } else {
            get_hdd_path(path, hdd_path);
            rename(ssd_path, hdd_path);
            truncate(hdd_path, size);
            unlink(ssd_path);
            symlink(hdd_path, ssd_path);
        }
        update_create_symlink_with_ssdpath(ssd_path, xattr_path);
    } else {
        if (xattr_exist(path)) {
            get_hddpath_from_ssdpath(ssd_path, hdd_path);
            rename(hdd_path, ssd_path);
            truncate(ssd_path, size);
            unlink(hdd_path);
            unlink(xattr_path);
        } else {
            truncate(ssd_path, size);
        }
    }
    printf("my_truncate: %s\n", path);
    return 0;
}

int my_open(const char *path, struct fuse_file_info *fi) {
    char ssd_path[256], hdd_path[256];
    get_ssd_path(path, ssd_path);
    int res;
    if (xattr_exist(path)) {
        get_hddpath_from_ssdpath(ssd_path, hdd_path);
        res = open(hdd_path, fi->flags);
    } else {
        res = open(ssd_path, fi->flags);
    }
    if (res == -1) return -errno;
    fi->fh = res;
    close(res);
    printf("my_open: %s\n", ssd_path);
    return 0;
}

int my_read(const char *path, char *buf, size_t size, off_t offset,
		     struct fuse_file_info *fi) {
    char ssd_path[256];
    get_ssd_path(path, ssd_path);
    int fd, res;
    // 如果是HDD文件
    if (xattr_exist(path)) {
        char hdd_path[256];
        get_hddpath_from_ssdpath(ssd_path, hdd_path);
        fd = open(hdd_path, O_RDONLY);
        if (fd == -1) return -errno;
        printf("my_read: %s %ld\n", hdd_path, size);
    } else {    // 如果是SSD文件
        fd = open(ssd_path, O_RDONLY);
        if (fd == -1) return -errno;
        printf("my_read: %s %ld\n", ssd_path, size);
    }
    res = pread(fd, buf, size, offset);
    if (res == -1) return -errno;
    buf[res] = 0;
    close(fd);
    return res;
}

int my_write(const char *path, const char *buf, size_t size, off_t offset,
		      struct fuse_file_info *fi) {
    (void)fi;
    char ssd_path[256];
    get_ssd_path(path, ssd_path);
    int fd, res;
    if (xattr_exist(path)) {    // 文件在HDD上
        char hdd_path[256], xattr_path[256];
        get_hddpath_from_ssdpath(ssd_path, hdd_path);
        fd = open(hdd_path, O_WRONLY);
        if (fd == -1) return -errno;
        res = pwrite(fd, buf, size, offset);
        if (res == -1) return -errno;
        printf("my_write: %s %ld\n", hdd_path, size);
        update_create_symlink_with_ssdpath(ssd_path, xattr_path);   // 更新元数据
    } else {
        struct stat st;
        res = stat(ssd_path, &st);
        if (res == -1) return -errno;
        if (st.st_size + size >= THRESH) {  // 如果超出阈值，先搬到HDD上
            char hdd_path[256], xattr_path[256];
            get_hdd_path(path, hdd_path);
            rename(ssd_path, hdd_path);
            unlink(ssd_path);
            symlink(hdd_path, ssd_path);
            fd = open(hdd_path, O_WRONLY);
            if (fd == -1) return -errno;
            res = pwrite(fd, buf, size, offset);
            if (res == -1) return -errno;
            printf("my_write: %s %ld\n", hdd_path, size);
            update_create_symlink_with_ssdpath(ssd_path, xattr_path);   // 创建+写入元数据
        } else {    // 未超出阈值，直接在SSD上写
            fd = open(ssd_path, O_WRONLY);
            if (fd == -1) return -errno;
            res = pwrite(fd, buf, size, offset);
            printf("my_write: %s %ld\n", ssd_path, size);
        }
    }
    close(fd);
    return res;
}

int my_utimens(const char *path, const struct timespec tv[2]) {
    char ssd_path[256];
    get_ssd_path(path, ssd_path);
    int res;
    if (xattr_exist(path)) {
        char hdd_path[256];
        get_hddpath_from_ssdpath(ssd_path, hdd_path);
        res = utimensat(0, hdd_path, tv, AT_SYMLINK_NOFOLLOW);
        if (res == -1) return -errno;
        printf("my_utimens: %s\n", hdd_path);
    } else {
        res = utimensat(0, ssd_path, tv, AT_SYMLINK_NOFOLLOW);
        if (res == -1) return -errno;
        printf("my_utimens: %s\n", ssd_path);
    }
    return 0;
}

struct fuse_operations ops = {
    .getattr = my_getattr,  
    .readdir = my_readdir,  
    .access = my_access,    
    .readlink = my_readlink,
    .symlink = my_symlink,
    .unlink = my_unlink,
    .create = my_create,
    .mknod = my_mknod,
    .mkdir = my_mkdir,      
    .rmdir = my_rmdir,      
    .rename = my_rename,
    .chmod = my_chmod,
    .chown = my_chown,  
    .truncate = my_truncate,
    .open = my_open,        
    .read = my_read,        
    .write = my_write,      
    .utimens = my_utimens,  
};

int main(int argc, char* argv[]) {
    return fuse_main(argc, argv, &ops, NULL);
}