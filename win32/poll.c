/* Emulation for poll(2)
   Contributed by Paolo Bonzini.

   Copyright 2001-2003, 2006-2024 Free Software Foundation, Inc.

   This file is part of gnulib.

   This file is free software: you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as
   published by the Free Software Foundation; either version 2.1 of the
   License, or (at your option) any later version.

   This file is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program.  If not, see <https://www.gnu.org/licenses/>.  */

/* Tell gcc not to warn about the (nfd < 0) tests, below.  */
#if (__GNUC__ == 4 && 3 <= __GNUC_MINOR__) || 4 < __GNUC__
# pragma GCC diagnostic ignored "-Wtype-limits"
#endif

#include "libbb.h"
#include <malloc.h>

#include <sys/types.h>

/* Specification.  */
#include <poll.h>

#include <errno.h>
#include <limits.h>
#include <assert.h>

#if defined _WIN32 && ! defined __CYGWIN__
# define WINDOWS_NATIVE
# include <winsock2.h>
# include <windows.h>
# include <io.h>
# include <stdio.h>
# include <conio.h>
#else
# include <sys/time.h>
# include <unistd.h>
#endif

#include <sys/select.h>
#include <sys/socket.h>

#ifdef HAVE_SYS_IOCTL_H
# include <sys/ioctl.h>
#endif
#ifdef HAVE_SYS_FILIO_H
# include <sys/filio.h>
#endif

#include <time.h>

#ifndef INFTIM
# define INFTIM (-1)
#endif

/* BeOS does not have MSG_PEEK.  */
#ifndef MSG_PEEK
# define MSG_PEEK 0
#endif

#ifdef WINDOWS_NATIVE

/* Don't assume that UNICODE is not defined.  */
# undef GetModuleHandle
# define GetModuleHandle GetModuleHandleA
# undef PeekConsoleInput
# define PeekConsoleInput PeekConsoleInputA
# undef CreateEvent
# define CreateEvent CreateEventA
# undef PeekMessage
# define PeekMessage PeekMessageA
# undef DispatchMessage
# define DispatchMessage DispatchMessageA

/* Do *not* use the function WSAPoll
   <https://docs.microsoft.com/en-us/windows/desktop/api/winsock2/nf-winsock2-wsapoll>
   because there is a bug named “Windows 8 Bugs 309411 - WSAPoll does not
   report failed connections” that Microsoft won't fix.
   See Daniel Stenberg: "WASPoll is broken"
   <https://daniel.haxx.se/blog/2012/10/10/wsapoll-is-broken/>.  */

/* Here we need the recv() function from Windows, that takes a SOCKET as
   first argument, not any possible gnulib override.  */
# undef recv

/* Here we need the select() function from Windows, because we pass bit masks
   of SOCKETs, not bit masks of FDs.  */
# undef select

/* Here we need timeval from Windows since this is what the select() function
   from Windows requires.  */
# undef timeval

/* Avoid warnings from gcc -Wcast-function-type.  */
# define GetProcAddress \
   (void *) GetProcAddress

static BOOL IsConsoleHandle (HANDLE h)
{
  DWORD mode;
  return GetConsoleMode (h, &mode) != 0;
}

static BOOL
IsSocketHandle (HANDLE h)
{
  WSANETWORKEVENTS ev;

  if (IsConsoleHandle (h))
    return FALSE;

  /* Under Wine, it seems that getsockopt returns 0 for pipes too.
     WSAEnumNetworkEvents instead distinguishes the two correctly.  */
  ev.lNetworkEvents = 0xDEADBEEF;
  WSAEnumNetworkEvents ((SOCKET) h, NULL, &ev);
  return ev.lNetworkEvents != 0xDEADBEEF;
}

/* Declare data structures for ntdll functions.  */
typedef struct _FILE_PIPE_LOCAL_INFORMATION {
  ULONG NamedPipeType;
  ULONG NamedPipeConfiguration;
  ULONG MaximumInstances;
  ULONG CurrentInstances;
  ULONG InboundQuota;
  ULONG ReadDataAvailable;
  ULONG OutboundQuota;
  ULONG WriteQuotaAvailable;
  ULONG NamedPipeState;
  ULONG NamedPipeEnd;
} FILE_PIPE_LOCAL_INFORMATION, *PFILE_PIPE_LOCAL_INFORMATION;

typedef struct _IO_STATUS_BLOCK
{
  union {
    DWORD Status;
    PVOID Pointer;
  } u;
  ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef enum _FILE_INFORMATION_CLASS {
  FilePipeLocalInformation = 24
} FILE_INFORMATION_CLASS, *PFILE_INFORMATION_CLASS;

typedef DWORD (WINAPI *PNtQueryInformationFile)
         (HANDLE, IO_STATUS_BLOCK *, VOID *, ULONG, FILE_INFORMATION_CLASS);

# ifndef PIPE_BUF
#  define PIPE_BUF      512
# endif

/* Compute revents values for file handle H.  If some events cannot happen
   for the handle, eliminate them from *P_SOUGHT.  */

static int
windows_compute_revents (HANDLE h, int *p_sought)
{
  int i, ret, happened;
  INPUT_RECORD *irbuffer;
  DWORD avail, nbuffer;
  BOOL bRet;
#if 0
  IO_STATUS_BLOCK iosb;
  FILE_PIPE_LOCAL_INFORMATION fpli;
  static PNtQueryInformationFile NtQueryInformationFile;
  static BOOL once_only;
#endif

  switch (GetFileType (h))
    {
    case FILE_TYPE_PIPE:
#if 0
      if (!once_only)
        {
          NtQueryInformationFile = (PNtQueryInformationFile)
            GetProcAddress (GetModuleHandle ("ntdll.dll"),
                            "NtQueryInformationFile");
          once_only = TRUE;
        }
#endif

      happened = 0;
      if (PeekNamedPipe (h, NULL, 0, NULL, &avail, NULL) != 0)
        {
          if (avail)
            happened |= *p_sought & (POLLIN | POLLRDNORM);
        }
      else if (GetLastError () == ERROR_BROKEN_PIPE)
        happened |= POLLHUP;

      else
        {
		/* The writability of a pipe can't be detected reliably on Windows.
		 * Just say it's OK.
		 *
		 * Details:
		 *
		 *    https://github.com/git-for-windows/git/commit/94f4d01932279c419844aa708bec31a26056bc6b
		 */
#if 0
          /* It was the write-end of the pipe.  Check if it is writable.
             If NtQueryInformationFile fails, optimistically assume the pipe is
             writable.  This could happen on Windows 9x, where
             NtQueryInformationFile is not available, or if we inherit a pipe
             that doesn't permit FILE_READ_ATTRIBUTES access on the write end
             (I think this should not happen since Windows XP SP2; WINE seems
             fine too).  Otherwise, ensure that enough space is available for
             atomic writes.  */
          memset (&iosb, 0, sizeof (iosb));
          memset (&fpli, 0, sizeof (fpli));

          if (!NtQueryInformationFile
              || NtQueryInformationFile (h, &iosb, &fpli, sizeof (fpli),
                                         FilePipeLocalInformation)
              || fpli.WriteQuotaAvailable >= PIPE_BUF
              || (fpli.OutboundQuota < PIPE_BUF &&
                  fpli.WriteQuotaAvailable == fpli.OutboundQuota))
#endif
            happened |= *p_sought & (POLLOUT | POLLWRNORM | POLLWRBAND);
        }
      return happened;

    case FILE_TYPE_CHAR:
      // Fall through to default case for non-console, e.g. /dev/null.
      if (IsConsoleHandle (h)) {
      nbuffer = avail = 0;
      bRet = GetNumberOfConsoleInputEvents (h, &nbuffer);
      if (bRet)
        {
          /* Input buffer.  */
          *p_sought &= POLLIN | POLLRDNORM;
          if (nbuffer == 0)
            // Having no unread events isn't an error condition.
            return 0 /* was POLLHUP */;
          if (!*p_sought)
            return 0;

          irbuffer = (INPUT_RECORD *) alloca (nbuffer * sizeof (INPUT_RECORD));
          bRet = PeekConsoleInputW (h, irbuffer, nbuffer, &avail);
          if (!bRet || avail == 0)
            return POLLHUP;

          for (i = 0; i < avail; i++)
            // Ignore key release.
            if (irbuffer[i].EventType == KEY_EVENT &&
                irbuffer[i].Event.KeyEvent.bKeyDown)
              return *p_sought;
          return 0;
        }
      else
        {
          /* Screen buffer.  */
          *p_sought &= POLLOUT | POLLWRNORM | POLLWRBAND;
          return *p_sought;
        }
      }
      /* fall through */

    default:
      ret = WaitForSingleObject (h, 0);
      if (ret == WAIT_OBJECT_0)
        return *p_sought & ~(POLLPRI | POLLRDBAND);

      // Add (POLLIN | POLLRDNORM).  Why only support write?
      return *p_sought & (POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM | POLLWRBAND);
    }
}

/* Convert fd_sets returned by select into revents values.  */

static int
windows_compute_revents_socket (SOCKET h, int sought, long lNetworkEvents)
{
  int happened = 0;

  if ((lNetworkEvents & (FD_READ | FD_ACCEPT | FD_CLOSE)) == FD_ACCEPT)
    happened |= (POLLIN | POLLRDNORM) & sought;

  else if (lNetworkEvents & (FD_READ | FD_ACCEPT | FD_CLOSE))
    {
      int r, error;

      char data[64];
      WSASetLastError (0);
      r = recv (h, data, sizeof (data), MSG_PEEK);
      error = WSAGetLastError ();
      WSASetLastError (0);

      if (r > 0 || error == WSAENOTCONN)
        happened |= (POLLIN | POLLRDNORM) & sought;

      /* Distinguish hung-up sockets from other errors.  */
      else if (r == 0 || error == WSAESHUTDOWN || error == WSAECONNRESET
               || error == WSAECONNABORTED || error == WSAENETRESET)
        happened |= POLLHUP;

      else
        happened |= POLLERR;
    }

  if (lNetworkEvents & (FD_WRITE | FD_CONNECT))
    happened |= (POLLOUT | POLLWRNORM | POLLWRBAND) & sought;

  if (lNetworkEvents & FD_OOB)
    happened |= (POLLPRI | POLLRDBAND) & sought;

  return happened;
}

#else /* !MinGW */

/* Convert select(2) returned fd_sets into poll(2) revents values.  */
static int
compute_revents (int fd, int sought, fd_set *rfds, fd_set *wfds, fd_set *efds)
{
  int happened = 0;
  if (FD_ISSET (fd, rfds))
    {
      int r;
      int socket_errno;

# if defined __MACH__ && defined __APPLE__
      /* There is a bug in Mac OS X that causes it to ignore MSG_PEEK
         for some kinds of descriptors.  Detect if this descriptor is a
         connected socket, a server socket, or something else using a
         0-byte recv, and use ioctl(2) to detect POLLHUP.  */
      r = recv (fd, NULL, 0, MSG_PEEK);
      socket_errno = (r < 0) ? errno : 0;
      if (r == 0 || socket_errno == ENOTSOCK)
        ioctl (fd, FIONREAD, &r);
# else
      char data[64];
      r = recv (fd, data, sizeof (data), MSG_PEEK);
      socket_errno = (r < 0) ? errno : 0;
# endif
      if (r == 0)
        happened |= POLLHUP;

      /* If the event happened on an unconnected server socket,
         that's fine. */
      else if (r > 0 || ( /* (r == -1) && */ socket_errno == ENOTCONN))
        happened |= (POLLIN | POLLRDNORM) & sought;

      /* Distinguish hung-up sockets from other errors.  */
      else if (socket_errno == ESHUTDOWN || socket_errno == ECONNRESET
               || socket_errno == ECONNABORTED || socket_errno == ENETRESET)
        happened |= POLLHUP;

      /* some systems can't use recv() on non-socket, including HP NonStop */
      else if (socket_errno == ENOTSOCK)
        happened |= (POLLIN | POLLRDNORM) & sought;

      else
        happened |= POLLERR;
    }

  if (FD_ISSET (fd, wfds))
    happened |= (POLLOUT | POLLWRNORM | POLLWRBAND) & sought;

  if (FD_ISSET (fd, efds))
    happened |= (POLLPRI | POLLRDBAND) & sought;

  return happened;
}
#endif /* !MinGW */

int
poll (struct pollfd *pfd, nfds_t nfd, int timeout)
{
#ifndef WINDOWS_NATIVE
  fd_set rfds, wfds, efds;
  struct timeval tv;
  struct timeval *ptv;
  int maxfd, rc;
  nfds_t i;

  if (nfd > INT_MAX)
    {
      errno = EINVAL;
      return -1;
    }
  /* Don't check directly for NFD greater than OPEN_MAX.  Any practical use
     of a too-large NFD is caught by one of the other checks below, and
     checking directly for getdtablesize is too much of a portability
     and/or performance and/or correctness hassle.  */

  /* EFAULT is not necessary to implement, but let's do it in the
     simplest case. */
  if (!pfd && nfd)
    {
      errno = EFAULT;
      return -1;
    }

  /* convert timeout number into a timeval structure */
  if (timeout == 0)
    {
      ptv = &tv;
      tv = (struct timeval) {0};
    }
  else if (timeout > 0)
    {
      ptv = &tv;
      tv = (struct timeval) {
        .tv_sec = timeout / 1000,
        .tv_usec = (timeout % 1000) * 1000
      };
    }
  else if (timeout == INFTIM)
    /* wait forever */
    ptv = NULL;
  else
    {
      errno = EINVAL;
      return -1;
    }

  /* create fd sets and determine max fd */
  maxfd = -1;
  FD_ZERO (&rfds);
  FD_ZERO (&wfds);
  FD_ZERO (&efds);
  for (i = 0; i < nfd; i++)
    {
      if (pfd[i].fd < 0)
        continue;
      if (maxfd < pfd[i].fd)
        {
          maxfd = pfd[i].fd;
          if (FD_SETSIZE <= maxfd)
            {
              errno = EINVAL;
              return -1;
            }
        }
      if (pfd[i].events & (POLLIN | POLLRDNORM))
        FD_SET (pfd[i].fd, &rfds);
      /* see select(2): "the only exceptional condition detectable
         is out-of-band data received on a socket", hence we push
         POLLWRBAND events onto wfds instead of efds. */
      if (pfd[i].events & (POLLOUT | POLLWRNORM | POLLWRBAND))
        FD_SET (pfd[i].fd, &wfds);
      if (pfd[i].events & (POLLPRI | POLLRDBAND))
        FD_SET (pfd[i].fd, &efds);
    }

  /* examine fd sets */
  rc = select (maxfd + 1, &rfds, &wfds, &efds, ptv);
  if (rc < 0)
    return rc;

  /* establish results */
  rc = 0;
  for (i = 0; i < nfd; i++)
    {
      pfd[i].revents = (pfd[i].fd < 0
                        ? 0
                        : compute_revents (pfd[i].fd, pfd[i].events,
                                           &rfds, &wfds, &efds));
      rc += pfd[i].revents != 0;
    }

  return rc;
#else
  static struct timeval tv0;
  static HANDLE hEvent;
  WSANETWORKEVENTS ev;
  HANDLE h, handle_array[FD_SETSIZE + 2];
  DWORD ret, wait_timeout, nhandles;
  fd_set rfds, wfds, xfds;
  BOOL poll_again;
  MSG msg;
  int rc = 0;
  nfds_t i;
  DWORD real_timeout = 0;
  int save_timeout = timeout;
  clock_t tend = clock () + timeout;

  if (nfd > INT_MAX || timeout < -1)
    {
      errno = EINVAL;
      return -1;
    }

  if (!hEvent)
    hEvent = CreateEvent (NULL, FALSE, FALSE, NULL);

restart:
  /* How much is left to wait? */
  timeout = save_timeout;
  if (timeout != INFTIM)
    {
      clock_t now = clock ();
      real_timeout = tend > now ? tend - now : 0;
    }

  handle_array[0] = hEvent;
  nhandles = 1;
  FD_ZERO (&rfds);
  FD_ZERO (&wfds);
  FD_ZERO (&xfds);

  /* Classify socket handles and create fd sets. */
  for (i = 0; i < nfd; i++)
    {
      int sought = pfd[i].events;
      pfd[i].revents = 0;
      if (pfd[i].fd < 0)
        continue;
      if (!(sought & (POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM | POLLWRBAND
                      | POLLPRI | POLLRDBAND)))
        continue;

      h = (HANDLE) _get_osfhandle (pfd[i].fd);
      assert (h != NULL);
      if (IsSocketHandle (h))
        {
          int requested = FD_CLOSE;

          /* see above; socket handles are mapped onto select.  */
          if (sought & (POLLIN | POLLRDNORM))
            {
              requested |= FD_READ | FD_ACCEPT;
              FD_SET ((SOCKET) h, &rfds);
            }
          if (sought & (POLLOUT | POLLWRNORM | POLLWRBAND))
            {
              requested |= FD_WRITE | FD_CONNECT;
              FD_SET ((SOCKET) h, &wfds);
            }
          if (sought & (POLLPRI | POLLRDBAND))
            {
              requested |= FD_OOB;
              FD_SET ((SOCKET) h, &xfds);
            }

          if (requested)
            WSAEventSelect ((SOCKET) h, hEvent, requested);
        }
      else
        {
          /* Poll now.  If we get an event, do not poll again.  Also,
             screen buffer handles are waitable, and they'll block until
             a character is available.  windows_compute_revents eliminates
             bits for the "wrong" direction. */
          pfd[i].revents = windows_compute_revents (h, &sought);
          if (sought)
            handle_array[nhandles++] = h;
          if (pfd[i].revents)
            timeout = 0;
        }
    }

  if (select (0, &rfds, &wfds, &xfds, &tv0) > 0)
    {
      /* Do MsgWaitForMultipleObjects anyway to dispatch messages, but
         no need to call select again.  */
      poll_again = FALSE;
      wait_timeout = 0;
    }
  else
    {
      poll_again = TRUE;
      if (timeout == INFTIM)
        wait_timeout = INFINITE;
      else
        wait_timeout = timeout;
    }

  for (;;)
    {
      ret = MsgWaitForMultipleObjects (nhandles, handle_array, FALSE,
                                       wait_timeout, QS_ALLINPUT);

      if (ret == WAIT_OBJECT_0 + nhandles)
        {
          /* new input of some other kind */
          BOOL bRet;
          while ((bRet = PeekMessage (&msg, NULL, 0, 0, PM_REMOVE)) != 0)
            {
              TranslateMessage (&msg);
              DispatchMessage (&msg);
            }
        }
      else
        break;
    }

  if (poll_again)
    select (0, &rfds, &wfds, &xfds, &tv0);

  /* Place a sentinel at the end of the array.  */
  handle_array[nhandles] = NULL;
  nhandles = 1;
  for (i = 0; i < nfd; i++)
    {
      int happened;

      if (pfd[i].fd < 0)
        continue;
      if (!(pfd[i].events & (POLLIN | POLLRDNORM |
                             POLLOUT | POLLWRNORM | POLLWRBAND)))
        continue;

      h = (HANDLE) _get_osfhandle (pfd[i].fd);
      if (h != handle_array[nhandles])
        {
          /* It's a socket.  */
          WSAEnumNetworkEvents ((SOCKET) h, NULL, &ev);
          WSAEventSelect ((SOCKET) h, 0, 0);

          /* If we're lucky, WSAEnumNetworkEvents already provided a way
             to distinguish FD_READ and FD_ACCEPT; this saves a recv later.  */
          if (FD_ISSET ((SOCKET) h, &rfds)
              && !(ev.lNetworkEvents & (FD_READ | FD_ACCEPT)))
            ev.lNetworkEvents |= FD_READ | FD_ACCEPT;
          if (FD_ISSET ((SOCKET) h, &wfds))
            ev.lNetworkEvents |= FD_WRITE | FD_CONNECT;
          if (FD_ISSET ((SOCKET) h, &xfds))
            ev.lNetworkEvents |= FD_OOB;

          happened = windows_compute_revents_socket ((SOCKET) h, pfd[i].events,
                                                     ev.lNetworkEvents);
        }
      else
        {
          /* Not a socket.  */
          int sought = pfd[i].events;
          happened = windows_compute_revents (h, &sought);
          nhandles++;
        }

       if ((pfd[i].revents |= happened) != 0)
        rc++;
    }

  if (!rc && (save_timeout == INFTIM || (real_timeout != 0 && nhandles > 1)))
    {
      SleepEx (1, TRUE);
      goto restart;
    }

  return rc;
#endif
}
