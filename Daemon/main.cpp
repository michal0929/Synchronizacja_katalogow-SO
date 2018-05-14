#include <iostream>
#include <cstring>
#include <set>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <iostream>
#include <vector>
#include <algorithm>
#include <signal.h>
#include <syslog.h>
#include <utime.h>
#include <bits/unique_ptr.h>
#include <sys/time.h>

std::string SOURCE;
std::string DESTINATION;
bool MODE;

struct tm* getTime() {
    time_t rawtime;
    struct tm * currentTime;

    time (&rawtime);
    currentTime = localtime (&rawtime);
    return currentTime;
}

std::unique_ptr<DIR, int (*)(DIR *)> openDir(std::string dirPath) {
    std::unique_ptr<DIR, decltype(&closedir)>
            dir(opendir(dirPath.c_str()), &closedir);
    return dir;
}

int isRegularFile(const std::string &path) {
//    struct stat sb;
//    return lstat(pathname.c_str(), &sb) == 0 && S_ISREG(sb.st_mode);
    struct stat path_stat;
    stat(path.c_str(), &path_stat);
    return S_ISREG(path_stat.st_mode) == 1? 1 : 0;
}

int isDir(const std::string &path) {
    struct stat path_stat;
    stat(path.c_str(), &path_stat);
    return S_ISDIR(path_stat.st_mode) == 1? 1 : 0;
}

int isDir(const std::string &parentDir, const std::string &nameCurrDir) {
    return isDir(parentDir + "/" + nameCurrDir);
}

int forceRemoveDir(const std::string &dirPath) {
    auto dir = openDir(dirPath);
    if (dir != nullptr) {
        struct dirent *entry;

        while ((entry = readdir(dir.get()))) {
            std::string entryName(entry->d_name);

            if ((entryName == ".") || (entryName == ".."))
                continue;

            std::string nextEntryPath = dirPath + "/" + entryName;
            if (isDir(dirPath, entryName)) {
                forceRemoveDir(nextEntryPath);
            }
            unlink(nextEntryPath.c_str());
        }
    } else {
        perror("Couldn't open the directory");
        return errno;
    }
    rmdir(dirPath.c_str());
    return 0;
}

int forceRemove(const std::string &entryPath) {

    if (isRegularFile(entryPath)) {
        // TODO: LOG removing regular file here

        syslog(LOG_NOTICE,"Delete regular file: %s. %s",entryPath.c_str(),asctime(getTime()));
        return unlink(entryPath.c_str());
    }
    // TODO: LOG removing dir here
    syslog(LOG_NOTICE,"Delete directory: %s. %s",entryPath.c_str(),asctime(getTime()));
    return forceRemoveDir(entryPath);
}

int copyFileByMmap(int srcFileDescriptor,
                   int dstFileDescriptor) {
    syslog(LOG_NOTICE," | mode: mmap  ");
    void *dstMmap;
    void *srcMmap;
    struct stat s;

    /* SOURCE */
    fstat(srcFileDescriptor, &s); // st_size = blocksize

    size_t size = static_cast<size_t>(s.st_size);
//    printf("%d\n", (int) s.st_size);

    srcMmap = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, srcFileDescriptor, 0);

    /* DESTINATION */

    ftruncate(dstFileDescriptor, size);
    dstMmap = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, dstFileDescriptor, 0);

    /* COPY */
    memcpy(dstMmap, srcMmap, size);
    munmap(srcMmap, size);
    munmap(dstMmap, size);

    return 0;
}

void oops(char *s1, char *s2) {
    fprintf(stderr, "Error: %s ", s1);
    perror(s2);
    exit(1);
}

int copyFileIO(int srcFileDescriptor, int dstFileDescriptor) {
    syslog(LOG_NOTICE," | mode: IO   ");
    struct stat stat;

    if (srcFileDescriptor == -1) {
        exit(EXIT_FAILURE);
    }

    if (fstat(srcFileDescriptor, &stat) == -1) {
        perror("fstat");
        exit(EXIT_FAILURE);
    }

    ssize_t n_chars;

    const size_t BUFFERSIZE = 8 * 1024;
    char buf[BUFFERSIZE];
    while ((n_chars = read(srcFileDescriptor, buf, BUFFERSIZE)) > 0) {
        if (write(dstFileDescriptor, buf, n_chars) != n_chars) {
            oops("Read error from ", "srcFile");
        }
        if (n_chars == -1) {
            oops("Error closing files", "dstFile");
        }
    }
    return EXIT_SUCCESS;
}

int copyFile(std::string srcPath, std::string dstPath, int thr) {

    if(!(isRegularFile(srcPath) )){
        std::cout << "No regular file: " << srcPath << std::endl;
        return -1;
    }

    if(!isRegularFile(dstPath)){
        std::cout << "No regular file: " << dstPath << std::endl;
        return -1;
    }
    int srcFileDescriptor = open(srcPath.c_str(), O_RDONLY);
    struct utimbuf times;

    time_t m_time;


    struct stat st;
    if(stat(srcPath.c_str(), &st) != 0)
        return -1;

    int dstFileDescriptor = open(dstPath.c_str(), O_RDWR | O_CREAT, st.st_mode);
    fchmod(dstFileDescriptor, st.st_mode);
    struct stat s = st;
    size_t size = static_cast<size_t>(s.st_size);

    syslog(LOG_NOTICE,"Copy file: %s. %s",srcPath.c_str(),asctime(getTime()));
    size < thr
    ? copyFileIO(srcFileDescriptor, dstFileDescriptor)
    : copyFileByMmap(srcFileDescriptor, dstFileDescriptor);

    close(srcFileDescriptor);
    close(dstFileDescriptor);

}

std::vector<std::string> getDiffEntries(const std::string &srcDirPath, const std::string &dstDirPath);

bool compareTimespec(const timespec &lhs, const timespec &rhs) {
    if (lhs.tv_sec == rhs.tv_sec)
        return lhs.tv_nsec < rhs.tv_nsec;
    else
        return lhs.tv_sec < rhs.tv_sec;
}

std::set<std::string> getAllEntriesFromDir(const std::string &dirPath) {
    std::set<std::string> files;
    auto dir = openDir(dirPath);
    if (dir != nullptr) {
        struct dirent *entry;
        while ((entry = readdir(dir.get()))) {
            std::string entryName(entry->d_name);
            if ((entryName == ".") || (entryName == ".."))
                continue;
            files.insert(std::move(entryName));
        }
    } else {
        perror("Couldn't open the directory");
    }
    return files;
}

int syncRecursive(const std::string &srcDirPath, const std::string &dstDirPath) {
    auto srcDir = openDir(srcDirPath);
    if (srcDir == nullptr) {
        std::string error("Couldn't open source directory: " +srcDirPath);
        perror(error.c_str());
        return errno;
    }

    auto dstDir = openDir(dstDirPath);
    if (dstDir == nullptr) {
        struct stat st;
        if(stat(srcDirPath.c_str(), &st) == 0)
            mkdir(dstDirPath.c_str(), st.st_mode);

        dstDir = openDir(dstDirPath);
        if (dstDir == nullptr) {
            std::string error("Couldn't open source directory: " +dstDirPath);
            perror(error.c_str());
            return errno;
        }
    }

    std::string tmpSrcDirPath(srcDirPath + "/");
    std::string tmpDstDirPath(dstDirPath + "/");

    for (const auto &name : getDiffEntries(srcDirPath, dstDirPath)) {
        forceRemove((tmpDstDirPath + name));
    }

    struct dirent *entry;
    while ((entry = readdir(srcDir.get()))) {
        std::string entryName(entry->d_name);

        if ((entryName == ".") || (entryName == ".."))
            continue;

        std::string nextSrcEntryPath(tmpSrcDirPath + entryName);
        std::string nextDstEntryPath(tmpDstDirPath + entryName);

        if (isDir(nextSrcEntryPath)) {
            syncRecursive(nextSrcEntryPath, nextDstEntryPath);
        } else if (isRegularFile(nextSrcEntryPath)) {

            struct stat srcStat;
            struct stat dstStat;
            int si = lstat(nextSrcEntryPath.c_str(), &srcStat);
            int di = lstat(nextDstEntryPath.c_str(), &dstStat);

            if(srcStat.st_mode != dstStat.st_mode)
            {
                chmod(nextDstEntryPath.c_str(),srcStat.st_mode);
                syslog(LOG_NOTICE,"Changed permmission in file %s. %s",nextDstEntryPath.c_str(), asctime(getTime()));
            }

            if (di == 0 && compareTimespec(srcStat.st_mtim, dstStat.st_mtim))
                continue;
            copyFile(nextSrcEntryPath, nextDstEntryPath, 8 * 1024*1024);

            struct timeval new_mtime[2];
            new_mtime[1].tv_sec = srcStat.st_mtim.tv_sec;
            new_mtime[1].tv_usec = srcStat.st_mtim.tv_nsec;
            new_mtime[0].tv_sec = srcStat.st_atim.tv_sec;
            new_mtime[0].tv_usec = srcStat.st_atim.tv_nsec;
            utimes(nextDstEntryPath.c_str(), new_mtime)<0;
        }
    }

    return 0;
}

int syncFlat(std::string srcDirPath, std::string dstDirPath) {
    auto srcDir = openDir(srcDirPath);
    if (srcDir == nullptr) {
        perror("Couldn't open source directory");
        return errno;
    }

    auto dstDir = openDir(dstDirPath);
    if (dstDir == nullptr) {
        struct stat st;
        if(stat(srcDirPath.c_str(), &st) == 0)
            mkdir(dstDirPath.c_str(), st.st_mode);

        dstDir = openDir(dstDirPath);
        if (dstDir == nullptr) {
            std::string error("Couldn't open source directory: " +dstDirPath);
            perror(error.c_str());
            return errno;
        }
    }


    srcDirPath += "/";
    dstDirPath += "/";

    for (const auto &name : getDiffEntries(srcDirPath, dstDirPath)) {
        std::string tmp;
        tmp += dstDirPath;
        tmp += name;
        if (isRegularFile(tmp))
            forceRemove((tmp));
    }

    struct dirent *entry;
    while ((entry = readdir(srcDir.get()))) {
        std::string entryName(entry->d_name);

        std::string nextSrcEntryPath(srcDirPath + entryName);
        std::string nextDstEntryPath(dstDirPath + entryName);

        if (isRegularFile(nextSrcEntryPath)) {

            struct stat srcStat;
            struct stat dstStat;
            int si = lstat(nextSrcEntryPath.c_str(), &srcStat);
            int di = lstat(nextDstEntryPath.c_str(), &dstStat);

            if(srcStat.st_mode != dstStat.st_mode)
            {
                chmod(nextDstEntryPath.c_str(),srcStat.st_mode);
                printf("Changed permmission in file %s\n",nextDstEntryPath.c_str());
            }

            if (di == 0 && compareTimespec(srcStat.st_mtim, dstStat.st_mtim))
                continue;
            copyFile(nextSrcEntryPath, nextDstEntryPath, 8 * 1024 * 1024);

            struct timeval new_mtime[2];
            new_mtime[1].tv_sec = srcStat.st_mtim.tv_sec;
            new_mtime[1].tv_usec = srcStat.st_mtim.tv_nsec;
            new_mtime[0].tv_sec = srcStat.st_atim.tv_sec;
            new_mtime[0].tv_usec = srcStat.st_atim.tv_nsec;
            utimes(nextDstEntryPath.c_str(), new_mtime)<0;
        }
    }
    return 0;
}

std::vector<std::string> getDiffEntries(const std::string &srcDirPath, const std::string &dstDirPath) {
    auto srcSet = getAllEntriesFromDir(srcDirPath);
    auto dstSet = getAllEntriesFromDir(dstDirPath);

    std::vector<std::__cxx11::string> toRemove;

    set_difference(
            dstSet.begin(), dstSet.end(),
            srcSet.begin(), srcSet.end(),
            back_inserter(toRemove)
    );
    return toRemove;
}

int sync(std::string srcDirPath, std::string dstDirPath, bool recursive = true) {

    if (srcDirPath[srcDirPath.size() - 1] == '/')
        srcDirPath.resize(srcDirPath.size() - 1);
    if (dstDirPath[dstDirPath.size() - 1] == '/')
        dstDirPath.resize(dstDirPath.size() - 1);

    if (recursive)
        syncRecursive((srcDirPath), (dstDirPath));
    else
        syncFlat((srcDirPath), dstDirPath);

}

void sig_handler(int signo)
{
    if (signo == SIGUSR1) {
        syslog (LOG_NOTICE, "Sync daemon triggered by SIGUSR1 %s", asctime(getTime()));
        sync(SOURCE, DESTINATION, MODE);
    }
}

int main(int argc, char **argv) {

    bool mode = false;

    std::string dstPath;
    std::string srcPath;
    int sleepTime;
    sleepTime = 300;
    openlog("MyDaemonLog", LOG_PID, LOG_DAEMON);
    if(argc ==0)
    {
        perror("Wrong Path:");
        EXIT_FAILURE;
    }
    if(!strcmp(argv[1],"-R"))
    {
        dstPath = argv[3];
        srcPath = argv[2];
        SOURCE = srcPath;
        DESTINATION = dstPath;
        mode = true;
        MODE = mode;
        if(argc == 5)
        {
            sleepTime = atoi(argv[4]);
        }

    } else {
        dstPath = argv[2];
        DESTINATION = dstPath;
        srcPath = argv[1];
        SOURCE = srcPath;
        MODE = false;
        if(argc == 4)
        {
            sleepTime = atoi(argv[3]);
        }
    }
    printf("%d\n",getpid()+1);
    syslog (LOG_NOTICE, "Sync daemon started recursiveMode = %d,"
                        " sleepTime = %d. %s",mode, sleepTime, asctime(getTime()));
    daemon(0,0);
    signal(SIGUSR1, sig_handler);
    while (1)
    {
        syslog(LOG_NOTICE, "Sync daemon triggered automatically. %s", asctime(getTime()));
        sync(srcPath,dstPath, mode);
        sleep(sleepTime);
    }


    return 0;
}
