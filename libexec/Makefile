#	@(#)Makefile	8.1 (Berkeley) 6/4/93
# $FreeBSD$

.include <src.opts.mk>

SUBDIR=	${_atf} \
	${_atrun} \
	${_casper} \
	${_cheritest_helper} \
	${_cheri_bench_helper} \
	${_comsat} \
	${_dma} \
	${_dma-mbox-create} \
	getty \
	${_libpng_sb_helper} \
	${_mail.local} \
	${_mknetid} \
	${_pppoed} \
	revnetgroup \
	${_rlogind} \
	rpc.rquotad \
	rpc.rstatd \
	rpc.rusersd \
	rpc.rwalld \
	rpc.sprayd \
	${_rshd} \
	${_rtld-elf} \
	save-entropy \
	${_smrsh} \
	${_tcpdump_helper} \
	${_telnetd} \
	${_tests} \
	${_tftp-proxy} \
	ulog-helper \
	${_ypxfr}

.if ${MK_AT} != "no"
_atrun=		atrun
.endif

.if ${MK_BOOTPD} != "no"
SUBDIR+=	bootpd
.endif

.if ${MK_CASPER} != "no"
_casper=	casper
.endif

.if ${MK_CHERI} != "no"
_cheri_bench_helper=	cheri_bench-helper
_cheritest_helper=	cheritest-helper
_libpng_sb_helper=	libpng_sb-helper
_tcpdump_helper=	tcpdump-helper
.endif

.if ${MK_FINGER} != "no"
SUBDIR+=	fingerd
.endif

.if ${MK_FTP} != "no"
SUBDIR+=	ftpd
.endif

.if ${MK_MAIL} != "no"
_comsat=	comsat
.endif

.if ${MK_DMAGENT} != "no"
_dma=		dma
_dma-mbox-create=	dma-mbox-create
.endif

.if ${MK_NIS} != "no"
_mknetid=	mknetid
_ypxfr=		ypxfr
.endif

.if ${MK_NETGRAPH} != "no"
_pppoed=	pppoed
.endif

.if ${MK_PF} != "no"
_tftp-proxy=	tftp-proxy
.endif

.if !defined(NO_PIC) && !defined(NO_RTLD)
_rtld-elf=	rtld-elf
.endif

.if ${MK_RBOOTD} != "no"
SUBDIR+=	rbootd
.endif

.if ${MK_RCMDS} != "no"
_rlogind=	rlogind
_rshd=		rshd
.endif

.if ${MK_SENDMAIL} != "no"
_mail.local=	mail.local
_smrsh=		smrsh
.endif

.if ${MK_TALK} != "no"
SUBDIR+=	talkd
.endif

.if ${MK_TCP_WRAPPERS} != "no"
SUBDIR+=	tcpd
.endif

.if ${MK_TELNET} != "no"
_telnetd=	telnetd
.endif

.if ${MK_TFTP} != "no"
SUBDIR+=	tftpd
.endif

.if ${MK_TESTS} != "no"
_atf=		atf
_tests=		tests
.endif

.include <bsd.arch.inc.mk>

.include <bsd.subdir.mk>
