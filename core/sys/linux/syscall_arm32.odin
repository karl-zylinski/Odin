#+build arm32
package linux

// This file was taken and transformed from
//   /arch/arm/tools/syscall.tbl
// in linux headers. OABI and EABI syscalls
// are included.

// TODO(bumbread, 2023-10-13): I'm not sure whether I have used
// the right syscall table. ARM64 stuff seems to be also compatible
// with ARM32, I'm not sure whether ARM32 also uses the new way of
// defining the syscall table, and this one is just for compatibility..?

// This syscall is not meant to be used by userspace
SYS_restart_syscall             :: uintptr(0)

SYS_exit                        :: uintptr(1)
SYS_fork                        :: uintptr(2)
SYS_read                        :: uintptr(3)
SYS_write                       :: uintptr(4)
SYS_open                        :: uintptr(5)
SYS_close                       :: uintptr(6)
// 7 was sys_waitpid
SYS_creat                       :: uintptr(8)
SYS_link                        :: uintptr(9)
SYS_unlink                      :: uintptr(10)
SYS_execve                      :: uintptr(11)
SYS_chdir                       :: uintptr(12)
SYS_time                        :: uintptr(13)
SYS_mknod                       :: uintptr(14)
SYS_chmod                       :: uintptr(15)
SYS_lchown                      :: uintptr(16)
// 17 was sys_break
// 18 was sys_stat
SYS_lseek                       :: uintptr(19)
SYS_getpid                      :: uintptr(20)
SYS_mount                       :: uintptr(21)
SYS_umount                      :: uintptr(22)
SYS_setuid                      :: uintptr(23)
SYS_getuid                      :: uintptr(24)
SYS_stime                       :: uintptr(25)
SYS_ptrace                      :: uintptr(26)
SYS_alarm                       :: uintptr(27)
// 28 was sys_fstat
SYS_pause                       :: uintptr(29)
SYS_utime                       :: uintptr(30)
// 31 was sys_stty
// 32 was sys_gtty
SYS_access                      :: uintptr(33)
SYS_nice                        :: uintptr(34)
// 35 was sys_ftime
SYS_sync                        :: uintptr(36)
SYS_kill                        :: uintptr(37)
SYS_rename                      :: uintptr(38)
SYS_mkdir                       :: uintptr(39)
SYS_rmdir                       :: uintptr(40)
SYS_dup                         :: uintptr(41)
SYS_pipe                        :: uintptr(42)
SYS_times                       :: uintptr(43)
// 44 was sys_prof
SYS_brk                         :: uintptr(45)
SYS_setgid                      :: uintptr(46)
SYS_getgid                      :: uintptr(47)
// 48 was sys_signal
SYS_geteuid                     :: uintptr(49)
SYS_getegid                     :: uintptr(50)
SYS_acct                        :: uintptr(51)
SYS_umount2                     :: uintptr(52)
// 53 was sys_lock
SYS_ioctl                       :: uintptr(54)
SYS_fcntl                       :: uintptr(55)
// 56 was sys_mpx
SYS_setpgid                     :: uintptr(57)
// 58 was sys_ulimit
// 59 was sys_olduname
SYS_umask                       :: uintptr(60)
SYS_chroot                      :: uintptr(61)
SYS_ustat                       :: uintptr(62)
SYS_dup2                        :: uintptr(63)
SYS_getppid                     :: uintptr(64)
SYS_getpgrp                     :: uintptr(65)
SYS_setsid                      :: uintptr(66)
SYS_sigaction                   :: uintptr(67)
// 68 was sys_sgetmask
// 69 was sys_ssetmask
SYS_setreuid                    :: uintptr(70)
SYS_setregid                    :: uintptr(71)
SYS_sigsuspend                  :: uintptr(72)
SYS_sigpending                  :: uintptr(73)
SYS_sethostname                 :: uintptr(74)
SYS_setrlimit                   :: uintptr(75)
// Back compat 2GB limited rlimit
SYS_getrlimit                   :: uintptr(76)
SYS_getrusage                   :: uintptr(77)
SYS_gettimeofday                :: uintptr(78)
SYS_settimeofday                :: uintptr(79)
SYS_getgroups                   :: uintptr(80)
SYS_setgroups                   :: uintptr(81)
SYS_select                      :: uintptr(82)
SYS_symlink                     :: uintptr(83)
// 84 was sys_lstat
SYS_readlink                    :: uintptr(85)
SYS_uselib                      :: uintptr(86)
SYS_swapon                      :: uintptr(87)
SYS_reboot                      :: uintptr(88)
SYS_readdir                     :: uintptr(89)
SYS_mmap                        :: uintptr(90)
SYS_munmap                      :: uintptr(91)
SYS_truncate                    :: uintptr(92)
SYS_ftruncate                   :: uintptr(93)
SYS_fchmod                      :: uintptr(94)
SYS_fchown                      :: uintptr(95)
SYS_getpriority                 :: uintptr(96)
SYS_setpriority                 :: uintptr(97)
// 98 was sys_profil
SYS_statfs                      :: uintptr(99)
SYS_fstatfs                     :: uintptr(100)
// 101 was sys_ioperm
SYS_socketcall                  :: uintptr(102)
SYS_syslog                      :: uintptr(103)
SYS_setitimer                   :: uintptr(104)
SYS_getitimer                   :: uintptr(105)
SYS_stat                        :: uintptr(106)
SYS_lstat                       :: uintptr(107)
SYS_fstat                       :: uintptr(108)
// 109 was sys_uname
// 110 was sys_iopl
SYS_vhangup                     :: uintptr(111)
// 112 was sys_idle
// syscall to call a syscall!
SYS_syscall                     :: uintptr(113)
SYS_wait4                       :: uintptr(114)
SYS_swapoff                     :: uintptr(115)
SYS_sysinfo                     :: uintptr(116)
SYS_ipc                         :: uintptr(117)
SYS_fsync                       :: uintptr(118)
SYS_sigreturn                   :: uintptr(119)
SYS_clone                       :: uintptr(120)
SYS_setdomainname               :: uintptr(121)
SYS_uname                       :: uintptr(122)
// 123 was sys_modify_ldt
SYS_adjtimex                    :: uintptr(124)
SYS_mprotect                    :: uintptr(125)
SYS_sigprocmask                 :: uintptr(126)
// 127 was sys_create_module
SYS_init_module                 :: uintptr(128)
SYS_delete_module               :: uintptr(129)
// 130 was sys_get_kernel_syms
SYS_quotactl                    :: uintptr(131)
SYS_getpgid                     :: uintptr(132)
SYS_fchdir                      :: uintptr(133)
SYS_bdflush                     :: uintptr(134)
SYS_sysfs                       :: uintptr(135)
SYS_personality                 :: uintptr(136)
// 137 was sys_afs_syscall
SYS_setfsuid                    :: uintptr(138)
SYS_setfsgid                    :: uintptr(139)
SYS__llseek                     :: uintptr(140)
SYS_getdents                    :: uintptr(141)
SYS__newselect                  :: uintptr(142)
SYS_flock                       :: uintptr(143)
SYS_msync                       :: uintptr(144)
SYS_readv                       :: uintptr(145)
SYS_writev                      :: uintptr(146)
SYS_getsid                      :: uintptr(147)
SYS_fdatasync                   :: uintptr(148)
SYS__sysctl                     :: uintptr(149)
SYS_mlock                       :: uintptr(150)
SYS_munlock                     :: uintptr(151)
SYS_mlockall                    :: uintptr(152)
SYS_munlockall                  :: uintptr(153)
SYS_sched_setparam              :: uintptr(154)
SYS_sched_getparam              :: uintptr(155)
SYS_sched_setscheduler          :: uintptr(156)
SYS_sched_getscheduler          :: uintptr(157)
SYS_sched_yield                 :: uintptr(158)
SYS_sched_get_priority_max      :: uintptr(159)
SYS_sched_get_priority_min      :: uintptr(160)
SYS_sched_rr_get_interval       :: uintptr(161)
SYS_nanosleep                   :: uintptr(162)
SYS_mremap                      :: uintptr(163)
SYS_setresuid                   :: uintptr(164)
SYS_getresuid                   :: uintptr(165)
// 166 was sys_vm86
// 167 was sys_query_module
SYS_poll                        :: uintptr(168)
SYS_nfsservctl                  :: uintptr(169)
SYS_setresgid                   :: uintptr(170)
SYS_getresgid                   :: uintptr(171)
SYS_prctl                       :: uintptr(172)
SYS_rt_sigreturn                :: uintptr(173)
SYS_rt_sigaction                :: uintptr(174)
SYS_rt_sigprocmask              :: uintptr(175)
SYS_rt_sigpending               :: uintptr(176)
SYS_rt_sigtimedwait             :: uintptr(177)
SYS_rt_sigqueueinfo             :: uintptr(178)
SYS_rt_sigsuspend               :: uintptr(179)
SYS_pread64                     :: uintptr(180)
SYS_pwrite64                    :: uintptr(181)
SYS_chown                       :: uintptr(182)
SYS_getcwd                      :: uintptr(183)
SYS_capget                      :: uintptr(184)
SYS_capset                      :: uintptr(185)
SYS_sigaltstack                 :: uintptr(186)
SYS_sendfile                    :: uintptr(187)
// 188 reserved
// 189 reserved
SYS_vfork                       :: uintptr(190)
// SuS compliant getrlimit
SYS_ugetrlimit                  :: uintptr(191)
SYS_mmap2                       :: uintptr(192)
SYS_truncate64                  :: uintptr(193)
SYS_ftruncate64                 :: uintptr(194)
SYS_stat64                      :: uintptr(195)
SYS_lstat64                     :: uintptr(196)
SYS_fstat64                     :: uintptr(197)
SYS_lchown32                    :: uintptr(198)
SYS_getuid32                    :: uintptr(199)
SYS_getgid32                    :: uintptr(200)
SYS_geteuid32                   :: uintptr(201)
SYS_getegid32                   :: uintptr(202)
SYS_setreuid32                  :: uintptr(203)
SYS_setregid32                  :: uintptr(204)
SYS_getgroups32                 :: uintptr(205)
SYS_setgroups32                 :: uintptr(206)
SYS_fchown32                    :: uintptr(207)
SYS_setresuid32                 :: uintptr(208)
SYS_getresuid32                 :: uintptr(209)
SYS_setresgid32                 :: uintptr(210)
SYS_getresgid32                 :: uintptr(211)
SYS_chown32                     :: uintptr(212)
SYS_setuid32                    :: uintptr(213)
SYS_setgid32                    :: uintptr(214)
SYS_setfsuid32                  :: uintptr(215)
SYS_setfsgid32                  :: uintptr(216)
SYS_getdents64                  :: uintptr(217)
SYS_pivot_root                  :: uintptr(218)
SYS_mincore                     :: uintptr(219)
SYS_madvise                     :: uintptr(220)
SYS_fcntl64                     :: uintptr(221)
// 222 for tux
// 223 is unused
SYS_gettid                      :: uintptr(224)
SYS_readahead                   :: uintptr(225)
SYS_setxattr                    :: uintptr(226)
SYS_lsetxattr                   :: uintptr(227)
SYS_fsetxattr                   :: uintptr(228)
SYS_getxattr                    :: uintptr(229)
SYS_lgetxattr                   :: uintptr(230)
SYS_fgetxattr                   :: uintptr(231)
SYS_listxattr                   :: uintptr(232)
SYS_llistxattr                  :: uintptr(233)
SYS_flistxattr                  :: uintptr(234)
SYS_removexattr                 :: uintptr(235)
SYS_lremovexattr                :: uintptr(236)
SYS_fremovexattr                :: uintptr(237)
SYS_tkill                       :: uintptr(238)
SYS_sendfile64                  :: uintptr(239)
SYS_futex                       :: uintptr(240)
SYS_sched_setaffinity           :: uintptr(241)
SYS_sched_getaffinity           :: uintptr(242)
SYS_io_setup                    :: uintptr(243)
SYS_io_destroy                  :: uintptr(244)
SYS_io_getevents                :: uintptr(245)
SYS_io_submit                   :: uintptr(246)
SYS_io_cancel                   :: uintptr(247)
SYS_exit_group                  :: uintptr(248)
SYS_lookup_dcookie              :: uintptr(249)
SYS_epoll_create                :: uintptr(250)
SYS_epoll_ctl                   :: uintptr(251)
SYS_epoll_wait                  :: uintptr(252)
SYS_remap_file_pages            :: uintptr(253)
// 254 for set_thread_area
// 255 for get_thread_area
SYS_set_tid_address             :: uintptr(256)
SYS_timer_create                :: uintptr(257)
SYS_timer_settime               :: uintptr(258)
SYS_timer_gettime               :: uintptr(259)
SYS_timer_getoverrun            :: uintptr(260)
SYS_timer_delete                :: uintptr(261)
SYS_clock_settime               :: uintptr(262)
SYS_clock_gettime               :: uintptr(263)
SYS_clock_getres                :: uintptr(264)
SYS_clock_nanosleep             :: uintptr(265)
SYS_statfs64                    :: uintptr(266)
SYS_fstatfs64                   :: uintptr(267)
SYS_tgkill                      :: uintptr(268)
SYS_utimes                      :: uintptr(269)
SYS_arm_fadvise64_64            :: uintptr(270)
SYS_pciconfig_iobase            :: uintptr(271)
SYS_pciconfig_read              :: uintptr(272)
SYS_pciconfig_write             :: uintptr(273)
SYS_mq_open                     :: uintptr(274)
SYS_mq_unlink                   :: uintptr(275)
SYS_mq_timedsend                :: uintptr(276)
SYS_mq_timedreceive             :: uintptr(277)
SYS_mq_notify                   :: uintptr(278)
SYS_mq_getsetattr               :: uintptr(279)
SYS_waitid                      :: uintptr(280)
SYS_socket                      :: uintptr(281)
SYS_bind                        :: uintptr(282)
SYS_connect                     :: uintptr(283)
SYS_listen                      :: uintptr(284)
SYS_accept                      :: uintptr(285)
SYS_getsockname                 :: uintptr(286)
SYS_getpeername                 :: uintptr(287)
SYS_socketpair                  :: uintptr(288)
SYS_send                        :: uintptr(289)
SYS_sendto                      :: uintptr(290)
SYS_recv                        :: uintptr(291)
SYS_recvfrom                    :: uintptr(292)
SYS_shutdown                    :: uintptr(293)
SYS_setsockopt                  :: uintptr(294)
SYS_getsockopt                  :: uintptr(295)
SYS_sendmsg                     :: uintptr(296)
SYS_recvmsg                     :: uintptr(297)
SYS_semop                       :: uintptr(298)
SYS_semget                      :: uintptr(299)
SYS_semctl                      :: uintptr(300)
SYS_msgsnd                      :: uintptr(301)
SYS_msgrcv                      :: uintptr(302)
SYS_msgget                      :: uintptr(303)
SYS_msgctl                      :: uintptr(304)
SYS_shmat                       :: uintptr(305)
SYS_shmdt                       :: uintptr(306)
SYS_shmget                      :: uintptr(307)
SYS_shmctl                      :: uintptr(308)
SYS_add_key                     :: uintptr(309)
SYS_request_key                 :: uintptr(310)
SYS_keyctl                      :: uintptr(311)
SYS_semtimedop                  :: uintptr(312)
SYS_vserver                     :: uintptr(313)
SYS_ioprio_set                  :: uintptr(314)
SYS_ioprio_get                  :: uintptr(315)
SYS_inotify_init                :: uintptr(316)
SYS_inotify_add_watch           :: uintptr(317)
SYS_inotify_rm_watch            :: uintptr(318)
SYS_mbind                       :: uintptr(319)
SYS_get_mempolicy               :: uintptr(320)
SYS_set_mempolicy               :: uintptr(321)
SYS_openat                      :: uintptr(322)
SYS_mkdirat                     :: uintptr(323)
SYS_mknodat                     :: uintptr(324)
SYS_fchownat                    :: uintptr(325)
SYS_futimesat                   :: uintptr(326)
SYS_fstatat64                   :: uintptr(327)
SYS_unlinkat                    :: uintptr(328)
SYS_renameat                    :: uintptr(329)
SYS_linkat                      :: uintptr(330)
SYS_symlinkat                   :: uintptr(331)
SYS_readlinkat                  :: uintptr(332)
SYS_fchmodat                    :: uintptr(333)
SYS_faccessat                   :: uintptr(334)
SYS_pselect6                    :: uintptr(335)
SYS_ppoll                       :: uintptr(336)
SYS_unshare                     :: uintptr(337)
SYS_set_robust_list             :: uintptr(338)
SYS_get_robust_list             :: uintptr(339)
SYS_splice                      :: uintptr(340)
SYS_arm_sync_file_range         :: uintptr(341)
SYS_tee                         :: uintptr(342)
SYS_vmsplice                    :: uintptr(343)
SYS_move_pages                  :: uintptr(344)
SYS_getcpu                      :: uintptr(345)
SYS_epoll_pwait                 :: uintptr(346)
SYS_kexec_load                  :: uintptr(347)
SYS_utimensat                   :: uintptr(348)
SYS_signalfd                    :: uintptr(349)
SYS_timerfd_create              :: uintptr(350)
SYS_eventfd                     :: uintptr(351)
SYS_fallocate                   :: uintptr(352)
SYS_timerfd_settime             :: uintptr(353)
SYS_timerfd_gettime             :: uintptr(354)
SYS_signalfd4                   :: uintptr(355)
SYS_eventfd2                    :: uintptr(356)
SYS_epoll_create1               :: uintptr(357)
SYS_dup3                        :: uintptr(358)
SYS_pipe2                       :: uintptr(359)
SYS_inotify_init1               :: uintptr(360)
SYS_preadv                      :: uintptr(361)
SYS_pwritev                     :: uintptr(362)
SYS_rt_tgsigqueueinfo           :: uintptr(363)
SYS_perf_event_open             :: uintptr(364)
SYS_recvmmsg                    :: uintptr(365)
SYS_accept4                     :: uintptr(366)
SYS_fanotify_init               :: uintptr(367)
SYS_fanotify_mark               :: uintptr(368)
SYS_prlimit64                   :: uintptr(369)
SYS_name_to_handle_at           :: uintptr(370)
SYS_open_by_handle_at           :: uintptr(371)
SYS_clock_adjtime               :: uintptr(372)
SYS_syncfs                      :: uintptr(373)
SYS_sendmmsg                    :: uintptr(374)
SYS_setns                       :: uintptr(375)
SYS_process_vm_readv            :: uintptr(376)
SYS_process_vm_writev           :: uintptr(377)
SYS_kcmp                        :: uintptr(378)
SYS_finit_module                :: uintptr(379)
SYS_sched_setattr               :: uintptr(380)
SYS_sched_getattr               :: uintptr(381)
SYS_renameat2                   :: uintptr(382)
SYS_seccomp                     :: uintptr(383)
SYS_getrandom                   :: uintptr(384)
SYS_memfd_create                :: uintptr(385)
SYS_bpf                         :: uintptr(386)
SYS_execveat                    :: uintptr(387)
SYS_userfaultfd                 :: uintptr(388)
SYS_membarrier                  :: uintptr(389)
SYS_mlock2                      :: uintptr(390)
SYS_copy_file_range             :: uintptr(391)
SYS_preadv2                     :: uintptr(392)
SYS_pwritev2                    :: uintptr(393)
SYS_pkey_mprotect               :: uintptr(394)
SYS_pkey_alloc                  :: uintptr(395)
SYS_pkey_free                   :: uintptr(396)
SYS_statx                       :: uintptr(397)
SYS_rseq                        :: uintptr(398)
SYS_io_pgetevents               :: uintptr(399)
SYS_migrate_pages               :: uintptr(400)
SYS_kexec_file_load             :: uintptr(401)
// 402 is unused
SYS_clock_gettime64             :: uintptr(403)
SYS_clock_settime64             :: uintptr(404)
SYS_clock_adjtime64             :: uintptr(405)
SYS_clock_getres_time64         :: uintptr(406)
SYS_clock_nanosleep_time64      :: uintptr(407)
SYS_timer_gettime64             :: uintptr(408)
SYS_timer_settime64             :: uintptr(409)
SYS_timerfd_gettime64           :: uintptr(410)
SYS_timerfd_settime64           :: uintptr(411)
SYS_utimensat_time64            :: uintptr(412)
SYS_pselect6_time64             :: uintptr(413)
SYS_ppoll_time64                :: uintptr(414)
SYS_io_pgetevents_time64        :: uintptr(416)
SYS_recvmmsg_time64             :: uintptr(417)
SYS_mq_timedsend_time64         :: uintptr(418)
SYS_mq_timedreceive_time64      :: uintptr(419)
SYS_semtimedop_time64           :: uintptr(420)
SYS_rt_sigtimedwait_time64      :: uintptr(421)
SYS_futex_time64                :: uintptr(422)
SYS_sched_rr_get_interval_time64:: uintptr(423)
SYS_pidfd_send_signal           :: uintptr(424)
SYS_io_uring_setup              :: uintptr(425)
SYS_io_uring_enter              :: uintptr(426)
SYS_io_uring_register           :: uintptr(427)
SYS_open_tree                   :: uintptr(428)
SYS_move_mount                  :: uintptr(429)
SYS_fsopen                      :: uintptr(430)
SYS_fsconfig                    :: uintptr(431)
SYS_fsmount                     :: uintptr(432)
SYS_fspick                      :: uintptr(433)
SYS_pidfd_open                  :: uintptr(434)
SYS_clone3                      :: uintptr(435)
SYS_close_range                 :: uintptr(436)
SYS_openat2                     :: uintptr(437)
SYS_pidfd_getfd                 :: uintptr(438)
SYS_faccessat2                  :: uintptr(439)
SYS_process_madvise             :: uintptr(440)
SYS_epoll_pwait2                :: uintptr(441)
SYS_mount_setattr               :: uintptr(442)
SYS_quotactl_fd                 :: uintptr(443)
SYS_landlock_create_ruleset     :: uintptr(444)
SYS_landlock_add_rule           :: uintptr(445)
SYS_landlock_restrict_self      :: uintptr(446)
// 447 reserved for memfd_secret
SYS_process_mrelease            :: uintptr(448)
SYS_futex_waitv                 :: uintptr(449)
SYS_set_mempolicy_home_node     :: uintptr(450)
SYS_cachestat                   :: uintptr(451)
SYS_fchmodat2                   :: uintptr(452)
