/*
 * tv_attach.h - totalview PMI attachment header
 * 
 * See: http://www-unix.mcs.anl.gov/mpi/mpi-debug/mpich-attach.txt
 *
 * Created: 02/2008 Frank Mietke <frank.mietke@s1998.tu-chemnitz.de>
 *
 * Distributed under the GNU Public License Version 2 or later (See LICENSE)
 */
#ifndef __tv_attach_h
#define __tv_attach_h

/* Functions to be called by starter */
int tv_startup(int ntasks);
void tv_accept_one(int n);
void tv_complete(void);

/* Called by totalview */
void MPIR_Breakpoint(void);

#endif

