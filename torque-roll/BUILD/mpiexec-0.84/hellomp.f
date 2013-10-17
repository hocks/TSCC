c
c hellomp.f - test mpi program with openmp directives.
c
c How I hate fortran.  Most of this is just trying to get a decent
c printf().
c
c $Id: hellomp.f 326 2006-01-24 21:35:26Z pw $
c
c Copyright (C) 2003 Ohio Supercomputer Center.
c Distributed under the GNU Public License Version 2 or later (See LICENSE)
c
        program hellomp
        implicit none
        include 'mpif.h'
        integer err, myid, numproc, i, tid
        integer nlen, mlen, alen, slen
        character*(1024) hostname, s, f

        integer iargc
        external iargc
        external getarg
        integer lnblnk
        external lnblnk
        integer digits
        external digits
        integer omp_get_thread_num
        external omp_get_thread_num

        call hostnm(hostname)
        call MPI_Init(err)
        call MPI_Comm_size(MPI_COMM_WORLD, numproc, err)
        call MPI_Comm_rank(MPI_COMM_WORLD, myid, err)
        mlen = digits(myid)
        nlen = digits(numproc)
        alen = digits(iargc())
        slen = lnblnk(hostname)
        write(f,'(a,i1,a,i1,a,i1,a,i1,a)')
     +    '(''hellof from '',i', mlen, ',''/'',i', nlen,
     +    ','' hostname '',a', slen, ','' with '',i',
     +    alen, ','' args: '',$)'
        write(*,f) myid, numproc, hostname, iargc()
        do i=1,iargc()
            call getarg(i,s)
            slen = lnblnk(s) + 1
            write(f,'(a,i1,a)') '(a', slen, '$)'
            write(*,f) s
        end do
        write(*,*)
c$omp parallel private(tid)
        tid = omp_get_thread_num()
        write(*,*) 'openmp thread ', tid
c$omp end parallel

        call MPI_Finalize(err)
        end

        function digits(n)
        implicit none
        integer n
        integer digits
        digits = 1
        if (n .ge. 10) then
            digits = 2
            if (n .ge. 100) then
                digits = 3
                if (n .ge. 1000) then
                    digits = 4
                endif
            endif
        endif
        end

