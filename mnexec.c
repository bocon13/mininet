/* mnexec: execution utility for mininet
 *
 * Starts up programs and does things that are slow or
 * difficult in Python, including:
 *
 *  - closing all file descriptors except stdin/out/error
 *  - detaching from a controlling tty using setsid
 *  - running in network and other namespaces
 *  - printing out the pid of a process so we can identify it later
 *  - attaching to namespace(s) and cgroup
 *  - setting RT scheduling
 *
 * Partially based on public domain setsid(1)
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <linux/sched.h>
#include <unistd.h>
#include <limits.h>
#include <syscall.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sched.h>
#include <ctype.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <assert.h>
#include <string.h>
#include <sys/stat.h>

#if !defined(VERSION)
#define VERSION "(devel)"
#endif


void usage(char *name)
{
    printf("Execution utility for Mininet\n\n"
           "Usage: %s [-cdmnPpu] [-a pid] [-g group] [-r rtprio] cmd args...\n\n"
           "Options:\n"
           "  -c: close all file descriptors except stdin/out/error\n"
           "  -d: detach from tty by calling setsid()\n"
           "  -m: run in a new mount namespace\n"
           "  -n: run in a new network namespace\n"
           "  -P: run in a new pid namespace (implies -m)\n"
           "  -u: run in a new UTS (ipc, hostname) namespace\n"
           "  -p: print ^A + pid\n"
           "  -a pid: attach to pid's namespaces\n"
           "  -g group: add to cgroup\n"
           "  -r rtprio: run with SCHED_RR (usually requires -g)\n"
           "  -v: print version\n",
           name);
}


#if !defined(setns)
int setns(int fd, int nstype)
{
    return syscall(__NR_setns, fd, nstype);
}
#endif

struct namespace {
    int type;
    const char *name;
};

/* List of namespaces supported by this command */
static struct namespace namespaces[] = {
    { CLONE_NEWNET, "net" },
    { CLONE_NEWPID, "pid" },
    { CLONE_NEWUTS, "uts" },
    { CLONE_NEWNS,  "mnt" }
};


/* Validate alphanumeric path foo1/bar2/baz */
void validate(char *path)
{
    char *s;
    for (s=path; *s; s++) {
        if (!isalnum(*s) && *s != '/') {
            fprintf(stderr, "invalid path: %s\n", path);
            exit(1);
        }
    }
}


/* Add our pid to cgroup */
void cgroup(char *gname)
{
    static char path[PATH_MAX];
    static char *groups[] = {
        "cpu", "cpuacct", "cpuset", NULL
    };
    char **gptr;
    pid_t pid = getpid();
    int count = 0;
    validate(gname);
    for (gptr = groups; *gptr; gptr++) {
        FILE *f;
        snprintf(path, PATH_MAX, "/sys/fs/cgroup/%s/%s/tasks",
                 *gptr, gname);
        f = fopen(path, "w");
        if (f) {
            count++;
            fprintf(f, "%d\n", pid);
            fclose(f);
        }
    }
    if (!count) {
        fprintf(stderr, "cgroup: could not add to cgroup %s\n",
            gname);
        exit(1);
    }
}


/* Attach to ns if present.
   Returns ns flag (> 0) on success, 0 if ns is not different from caller's, and < 0 on err */
int attachns(pid_t pid, const struct namespace *ns) {
    char path[PATH_MAX], self[PATH_MAX];
    int nsid, err;
    int pidns = 0;

    snprintf(path, PATH_MAX, "/proc/%d/ns/%s", pid, ns->name);
    snprintf(self, PATH_MAX, "/proc/self/ns/%s", ns->name);

    struct stat buf1, buf2;
    int stat1 = stat(path, &buf1);
    int stat2 = stat(self, &buf2);
    /* Don't reattach to the same ns */
    if (stat1 == 0 && stat2 == 0 &&
        buf1.st_dev == buf2.st_dev &&
        buf1.st_ino == buf2.st_ino)
        return 0;

    if ((nsid = open(path, O_RDONLY)) < 0) {
        perror("open");
        fprintf(stderr, "Could not open: %s\n", path);
        return nsid;
    }

    if ((err = setns(nsid, 0)) < 0) {
        perror("setns");
        fprintf(stderr, "Could not attach to %s namespace\n", ns->name);
        return err;
    }

    return ns->type;
}

/* Attach to pid's namespaces; returns flags of mounted namespaces */
int attach(int pid) {
    char *cwd = get_current_dir_name();
    char path[PATH_MAX];
    int flag = 0, ret = 0;

    int len = sizeof(namespaces) / sizeof(struct namespace);
    for (struct namespace *ns = namespaces; ns < namespaces + len; ns++) {
        if ((ret = attachns(pid, ns)) < 0) {
            if (ns->type == CLONE_NEWNS) {
                /* Plan B: chroot into pid's root file system */
                sprintf(path, "/proc/%d/root", pid);
                if (chroot(path) < 0) {
                    perror(path);
                    return -1;
                }
            }
            else return -1;
        }
        flag |= ret;
    }

    /* chdir to correct working directory */
    if (chdir(cwd) != 0) {
        perror(cwd);
        return -1;
    }

    return flag;
}


int main(int argc, char *argv[])
{
    int c;

    /* Argument flags */
    int flags = 0;
    int closefds = 0;
    int attachpid = 0;
    char *cgrouparg = NULL;
    int detachtty = 0;
    int printpid = 0;
    int rtprio = 0;
    int dofork = 0;

    while ((c = getopt(argc, argv, "+cdmnPpa:g:r:uvh")) != -1)
        switch(c) {
            case 'c':   closefds = 1; break;
            case 'd':   detachtty = 1; break;
            case 'm':   flags |= CLONE_NEWNS; break;
            case 'n':   flags |= CLONE_NEWNET | CLONE_NEWNS; break;
            case 'p':   printpid = 1; break;
            case 'P':   flags |= CLONE_NEWPID | CLONE_NEWNS; break;
            case 'a':   attachpid = atoi(optarg);break;
            case 'g':   cgrouparg = optarg ; break;
            case 'r':   rtprio = atoi(optarg); break;
            case 'u':   flags |= CLONE_NEWUTS; break;
            case 'v':   printf("%s\n", VERSION); exit(0);
            case 'h':   usage(argv[0]); exit(0);
            default:    usage(argv[0]); exit(1);
    }

    if (closefds) {
        /* close file descriptors except stdin/out/error */
        int fd;
        for (fd = getdtablesize(); fd > 2; fd--) close(fd);
    }

    if (attachpid) {
        /* Attach to existing namespace(s) */
        if((flags = attach(attachpid)) == -1)
            return 1;
    }
    else {
        /* Create new namespace(s) */
        if (unshare(flags) == -1) {
            perror("unshare");
            return 1;
        }
    }

    if (flags & CLONE_NEWPID)
        /* pidns requires fork/wait; child will be pid 1 */
        dofork = 1;

    if (detachtty && getpgrp() == getpid())
        /* Fork so that we will no longer be pgroup leader */
        dofork = 1;
    else
        /* We don't need a new session, only a new pgroup */
        detachtty = 0;

    if (detachtty)
        /* Create a new session - and by implication a new process group */
        setsid();
    else
        /* Use a new process group (in the current session)
         * so Mininet can use killpg without unintended effects */
        setpgid(0, 0);

    if (dofork) {
        /* Fork and then wait if necessary */
        pid_t pid = fork();
        switch(pid) {
            int status;
            case -1:
                perror("fork");
                return 1;
            case 0:
                /* Child continues below */
                break;
            default:
                /* We print the *child pid* in *parent's pidns* if needed */
                if (printpid) {
                    printf("\001%d\n", pid);
                    fflush(stdout);
                }
                /* For pid namespace, we need to fork and wait for child ;-( */
                if (flags & CLONE_NEWPID) {
                     wait(&status);
                }
                return 0;
        }
    }

    if (printpid && !dofork) {
        /* Print child pid if we didn't fork/aren't in a pidns */
        assert(!(flags & CLONE_NEWPID));
        printf("\001%d\n", getpid());
        fflush(stdout);
    }


    /* Attach to cgroup if necessary */
    if (cgrouparg) cgroup(cgrouparg);

    if (!attachpid && flags & CLONE_NEWNS) {
        /* Set the whole hierarchy propagation to private */
        if (mount("none", "/", NULL, MS_REC|MS_PRIVATE, NULL) == -1) {
            perror("set / propagation to private");
            return 1;
        }
        if (flags & CLONE_NEWNET) {
            /* Mount sysfs to pick up the new network namespace */
            if (mount("sysfs", "/sys", "sysfs", MS_MGC_VAL, NULL) == -1) {
                perror("mount /sys");
                return 1;
            }
        }
        if (flags & CLONE_NEWPID) {
            /* Child remounts /proc for ps */
            if (mount("proc", "/proc", "proc", MS_NOSUID|MS_NOEXEC|MS_NODEV, NULL) == -1) {
                perror("mount /proc");
                return 1;
            }
        }
    }


    if (rtprio != 0) {
        /* Set RT scheduling priority */
        static struct sched_param sp;
        sp.sched_priority = atoi(optarg);
        if (sched_setscheduler(getpid(), SCHED_RR, &sp) < 0) {
            perror("sched_setscheduler");
            return 1;
        }
    }

    if (optind < argc) {
        execvp(argv[optind], &argv[optind]);
        perror(argv[optind]);
        return 1;
    }

    usage(argv[0]);

    return 0;
}
