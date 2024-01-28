KMOD=	tarfs
SRCS=	opt_tarfs.h \
	tarfs_io.c \
	tarfs_subr.c \
	tarfs_vnops.c \
	tarfs_vfsops.c

.ifdef TARFS_DEBUG
CFLAGS+=	-DTARFS_DEBUG
.endif

.include <bsd.kmod.mk>
