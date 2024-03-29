/* ply-utils.c -  random useful functions and macros
 *
 * Copyright (C) 2007 Red Hat, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 * Written by: Ray Strode <rstrode@redhat.com>
 */
#include <config.h>

#include "ply-utils.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <linux/fs.h>
#include <linux/vt.h>

#include <dlfcn.h>

#include "ply-logger.h"

#ifndef PLY_OPEN_FILE_DESCRIPTORS_DIR
#define PLY_OPEN_FILE_DESCRIPTORS_DIR "/proc/self/fd"
#endif

#ifndef PLY_ERRNO_STACK_SIZE
#define PLY_ERRNO_STACK_SIZE 256
#endif

#ifndef PLY_SOCKET_CONNECTION_BACKLOG
#define PLY_SOCKET_CONNECTION_BACKLOG 32
#endif

#ifndef PLY_SUPER_SECRET_LAZY_UNMOUNT_FLAG
#define PLY_SUPER_SECRET_LAZY_UNMOUNT_FLAG 2
#endif

#ifndef PLY_DISABLE_CONSOLE_PRINTK
#define PLY_DISABLE_CONSOLE_PRINTK 6
#endif

#ifndef PLY_ENABLE_CONSOLE_PRINTK
#define PLY_ENABLE_CONSOLE_PRINTK 7
#endif

static int errno_stack[PLY_ERRNO_STACK_SIZE];
static int errno_stack_position = 0;

bool 
ply_open_unidirectional_pipe (int *sender_fd,
                              int *receiver_fd)
{
  int pipe_fds[2];

  assert (sender_fd != NULL);
  assert (receiver_fd != NULL);

  if (pipe (pipe_fds) < 0)
    return false;

  if (fcntl (pipe_fds[0], F_SETFD, O_NONBLOCK | FD_CLOEXEC) < 0)
    {
      ply_save_errno ();
      close (pipe_fds[0]);
      close (pipe_fds[1]);
      ply_restore_errno ();
      return false;
    }

  if (fcntl (pipe_fds[1], F_SETFD, O_NONBLOCK | FD_CLOEXEC) < 0)
    {
      ply_save_errno ();
      close (pipe_fds[0]);
      close (pipe_fds[1]);
      ply_restore_errno ();
      return false;
    }

  *sender_fd = pipe_fds[1];
  *receiver_fd = pipe_fds[0];

  return true;
}

static int
ply_open_unix_socket (void)
{
  int fd;
  const int should_pass_credentials = true;

  fd = socket (PF_UNIX, SOCK_STREAM, 0);

  if (fd < 0)
    return -1;

  if (fcntl (fd, F_SETFD, O_NONBLOCK | FD_CLOEXEC) < 0)
    {
      ply_save_errno ();
      close (fd);
      ply_restore_errno ();

      return -1;
    }

  if (setsockopt (fd, SOL_SOCKET, SO_PASSCRED,
                  &should_pass_credentials, sizeof (should_pass_credentials)) < 0)
    {
      ply_save_errno ();
      close (fd);
      ply_restore_errno ();
      return -1;
    }

  return fd;
}

static struct sockaddr *
create_unix_address_from_path (const char *path,
                               bool        is_abstract,
                               size_t     *address_size)
{
  struct sockaddr_un *address; 

  assert (path != NULL && path[0] != '\0');
  assert (strlen (path) < sizeof (address->sun_path));

  address = calloc (1, sizeof (struct sockaddr_un));
  address->sun_family = AF_UNIX;

  /* a socket is marked as abstract if its path has the
   * NUL byte at the beginning of the buffer instead of
   * the end
   * 
   * Note, we depend on the memory being zeroed by the calloc
   * call above.
   */
  if (!is_abstract)
    strncpy (address->sun_path, path, sizeof (address->sun_path) - 1);
  else
    strncpy (address->sun_path + 1, path, sizeof (address->sun_path) - 1);

  if (address_size != NULL)
    *address_size = sizeof (struct sockaddr_un);

  return (struct sockaddr *) address;
}

int
ply_connect_to_unix_socket (const char *path,
                            bool        is_abstract)
{
  struct sockaddr *address; 
  size_t address_size;
  int fd;

  assert (path != NULL);
  assert (path[0] != '\0');
  
  fd = ply_open_unix_socket ();

  if (fd < 0)
    return -1;

  address = create_unix_address_from_path (path, is_abstract, &address_size);

  if (connect (fd, address, address_size) < 0)
    {
      ply_save_errno ();
      free (address);
      close (fd);
      ply_restore_errno ();

      return -1;
    }
  free (address);

  return fd;
}

int
ply_listen_to_unix_socket (const char *path,
                           bool        is_abstract)
{
  struct sockaddr *address; 
  size_t address_size;
  int fd;
  
  assert (path != NULL);
  assert (path[0] != '\0');

  fd = ply_open_unix_socket ();

  if (fd < 0)
    return -1;

  address = create_unix_address_from_path (path, is_abstract, &address_size);

  if (bind (fd, address, address_size) < 0)
    {
      ply_save_errno ();
      free (address);
      close (fd);
      ply_restore_errno ();

      return -1;
    }

  free (address);

  if (listen (fd, PLY_SOCKET_CONNECTION_BACKLOG) < 0)
    {
      ply_save_errno ();
      close (fd);
      ply_restore_errno ();
      return -1;
    }

  if (!is_abstract)
    {
      if (fchmod (fd, 0600) < 0)
        {
          ply_save_errno ();
          close (fd);
          ply_restore_errno ();
          return -1;
        }
    }

  return fd;
}

bool
ply_get_credentials_from_fd (int    fd,
                             pid_t *pid,
                             uid_t *uid,
                             gid_t *gid)
{
  struct ucred credentials;
  socklen_t credential_size;

  credential_size = sizeof (credentials);
  if (getsockopt (fd, SOL_SOCKET, SO_PEERCRED, &credentials,
                  &credential_size) < 0)
    return false;

  if (credential_size < sizeof (credentials))
    return false;

  if (pid != NULL)
    *pid = credentials.pid;

  if (uid != NULL)
    *uid = credentials.uid;

  if (gid != NULL)
    *gid = credentials.gid;

  return true;
}

bool 
ply_write (int         fd,
           const void *buffer,
           size_t      number_of_bytes)
{
  size_t bytes_left_to_write;
  size_t total_bytes_written = 0;
  int is_a_socket = 1;

  assert (fd >= 0);

  bytes_left_to_write = number_of_bytes;

  do
    {
      ssize_t bytes_written = 0;

      if (is_a_socket)
        bytes_written = send  (fd,
                               ((uint8_t *) buffer) + total_bytes_written,
                               bytes_left_to_write,
                               0
#if HAVE_DECL_MSG_NOSIGNAL
                               |MSG_NOSIGNAL
#endif
                               );
      else
        bytes_written = write (fd,
                               ((uint8_t *) buffer) + total_bytes_written,
                               bytes_left_to_write);

      if (bytes_written > 0)
        {
          total_bytes_written += bytes_written;
          bytes_left_to_write -= bytes_written;
        }
      else if ((errno == ENOTSOCK))
        is_a_socket = 0;
      else if ((errno != EINTR))
        break;
    }
  while (bytes_left_to_write > 0);

  return bytes_left_to_write == 0;
}

bool 
ply_write_uint32 (int      fd,
                  uint32_t value)
{
  uint8_t buffer[4];
  
  buffer[0] = (value >> 0) & 0xFF;
  buffer[1] = (value >> 8) & 0xFF;
  buffer[2] = (value >> 16) & 0xFF;
  buffer[3] = (value >> 24) & 0xFF;
  
  return ply_write (fd, buffer, 4 * sizeof (uint8_t));
}

static ssize_t
ply_read_some_bytes (int     fd,
                     void   *buffer,
                     size_t  max_bytes)
{
  size_t bytes_left_to_read;
  size_t total_bytes_read = 0;

  assert (fd >= 0);

  bytes_left_to_read = max_bytes;

  do
    {
      ssize_t bytes_read = 0;

      bytes_read = read (fd,
                         ((uint8_t *) buffer) + total_bytes_read,
                         bytes_left_to_read);

      if (bytes_read > 0)
        {
          total_bytes_read += bytes_read;
          bytes_left_to_read -= bytes_read;
        }
      else if ((errno != EINTR))
        break;
    }
  while (bytes_left_to_read > 0);

  if ((bytes_left_to_read > 0) && (errno != EAGAIN))
    total_bytes_read = -1;

  return total_bytes_read;
}

bool 
ply_read (int     fd,
          void   *buffer,
          size_t  number_of_bytes)
{
  size_t total_bytes_read;
  bool read_was_successful;

  assert (fd >= 0);
  assert (buffer != NULL);
  assert (number_of_bytes != 0);

  total_bytes_read = ply_read_some_bytes (fd, buffer, number_of_bytes);

  read_was_successful = total_bytes_read == number_of_bytes;

  return read_was_successful;
}

bool 
ply_read_uint32 (int       fd,
                 uint32_t *value)
{
  uint8_t buffer[4];
  
  if (!ply_read (fd, buffer, 4 * sizeof (uint8_t)))
    return false;
  
  *value = (buffer[0] << 0) | 
           (buffer[1] << 8) | 
           (buffer[2] << 16) | 
           (buffer[3] << 24);
  return true;
}

bool 
ply_fd_has_data (int fd)
{
  struct pollfd poll_data;
  int result;

  poll_data.fd = fd;
  poll_data.events = POLLIN | POLLPRI;
  poll_data.revents = 0;
  result = poll (&poll_data, 1, 10);

  return result == 1 
         && ((poll_data.revents & POLLIN) 
         || (poll_data.revents & POLLPRI));
}

bool 
ply_fd_can_take_data (int fd)
{
  struct pollfd poll_data;
  int result;

  poll_data.fd = fd;
  poll_data.events = POLLOUT;
  poll_data.revents = 0;
  result = poll (&poll_data, 1, 10);

  return result == 1;
}

bool
ply_fd_may_block (int fd)
{
  int flags;

  assert (fd >= 0);

  flags = fcntl (fd, F_GETFL);

  return (flags & O_NONBLOCK) != 0;
}

char **
ply_copy_string_array (const char * const *array)
{
  char **copy;
  int i;

  for (i = 0; array[i] != NULL; i++);

  copy = calloc (i + 1, sizeof (char *));

  for (i = 0; array[i] != NULL; i++)
    copy[i] = strdup (array[i]);

  return copy;
}

void 
ply_free_string_array (char **array)
{
  int i;

  if (array == NULL)
    return;

  for (i = 0; array[i] != NULL; i++)
    {
      free (array[i]);
      array[i] = NULL;
    }

  free (array);
}

bool
ply_string_has_prefix (const char *string,
                       const char *prefix)
{
  if (string == NULL)
    return false;

  if (prefix == NULL)
    return false;

  if (strlen (prefix) > strlen (string))
    return false;

  return strncmp (string, prefix, strlen (prefix)) == 0;
}

static int
ply_get_max_open_fds (void)
{
  struct rlimit open_fd_limit;

  if (getrlimit (RLIMIT_NOFILE, &open_fd_limit) < 0) 
    return -1;

  if (open_fd_limit.rlim_cur == RLIM_INFINITY) 
    return -1;

  return (int) open_fd_limit.rlim_cur;
}

static bool
ply_close_open_fds (void)
{
  DIR *dir;
  struct dirent *entry;
  int fd, opendir_fd;

  opendir_fd = -1;
  dir = opendir (PLY_OPEN_FILE_DESCRIPTORS_DIR);

  if (dir == NULL)
    return false;

  while ((entry = readdir (dir)) != NULL) 
    {
      long filename_as_number;
      char *byte_after_number;

      errno = 0;
      if (entry->d_name[0] == '.')
        continue;

      fd = -1;
      filename_as_number = strtol (entry->d_name, &byte_after_number, 10);

      if (byte_after_number != NULL)
        continue;

      if ((*byte_after_number != '\0') ||
          (filename_as_number < 0) ||
          (filename_as_number > INT_MAX)) 
        return false;

      fd = (int) filename_as_number;

      if (fd != opendir_fd)
        close (fd);
    }

  assert (entry == NULL);
  closedir (dir);

  return true;
}

void 
ply_close_all_fds (void)
{
  int max_open_fds, fd;

  max_open_fds = ply_get_max_open_fds ();

  /* if there isn't a reported maximum for some
   * reason, then open up /proc/self/fd and close
   * the ones we can find.  If that doesn't work
   * out, then just bite the bullet and close the
   * entire integer range
   */
  if (max_open_fds < 0)
    {
      if (ply_close_open_fds ())
        return;

      max_open_fds = INT_MAX;
    }

  else for (fd = 0; fd < max_open_fds; fd++) 
    close (fd);
}

double 
ply_get_timestamp (void)
{
  const double nanoseconds_per_second = 1000000000.0;
  double timestamp;
  struct timespec now = { 0L, /* zero-filled */ };

  clock_gettime (CLOCK_MONOTONIC, &now);
  timestamp = ((nanoseconds_per_second * now.tv_sec) + now.tv_nsec) /
               nanoseconds_per_second;

  return timestamp;
}

void 
ply_save_errno (void)
{
  assert (errno_stack_position < PLY_ERRNO_STACK_SIZE);
  errno_stack[errno_stack_position] = errno;
  errno_stack_position++;
}

void
ply_restore_errno (void)
{
  assert (errno_stack_position > 0);
  errno_stack_position--;
  errno = errno_stack[errno_stack_position];
}

bool 
ply_directory_exists (const char *dir)
{
  struct stat file_info;
  
  if (stat (dir, &file_info) < 0)
    return false;

  return S_ISDIR (file_info.st_mode);
}

bool
ply_file_exists (const char *file)
{
  struct stat file_info;
  
  if (stat (file, &file_info) < 0)
    return false;

  return S_ISREG (file_info.st_mode);
}

void 
ply_list_directory (const char *path)
{
  DIR *dir;
  struct dirent *entry;
  static int level = 0;

  dir = opendir (path);

  if (dir == NULL)
    return;

  if (level > 5)
    return;

  int index = 0;
  while ((entry = readdir (dir)) != NULL) 
    {
      char *subdir;

      index++;

      if (index > 10)
        break;

      subdir = NULL;
      asprintf (&subdir, "%s/%s", path, entry->d_name);
      ply_error ("%s ", subdir);
      level++;
      if (entry->d_name[0] != '.')
        ply_list_directory (subdir);
      level--;
      free (subdir);
    }

  closedir (dir);
}

ply_module_handle_t *
ply_open_module (const char *module_path)
{
  ply_module_handle_t *handle;

  assert (module_path != NULL);

  handle = (ply_module_handle_t *) dlopen (module_path,
                                           RTLD_NODELETE |RTLD_NOW | RTLD_LOCAL);

  if (handle == NULL)
    {
      ply_trace("Could not load module \"%s\": %s\n", module_path, dlerror());
      if (errno == 0)
        errno = ELIBACC;
    }

  return handle;
}

ply_module_function_t
ply_module_look_up_function (ply_module_handle_t *handle,
                             const char          *function_name)
{
  ply_module_function_t function;

  assert (handle != NULL);
  assert (function_name != NULL);

  dlerror ();
  function = (ply_module_function_t) dlsym (handle, function_name);

  if (dlerror () != NULL)
    {
      if (errno == 0)
        errno = ELIBACC;

      return NULL;
    }

  return function;
}

void
ply_close_module (ply_module_handle_t *handle)
{
  dlclose (handle);
}

bool
ply_create_directory (const char *directory)
{
  assert (directory != NULL);
  assert (directory[0] != '\0');

  if (ply_directory_exists (directory))
    {
      ply_trace ("directory '%s' already exists", directory);
      return true;
    }

  if (ply_file_exists (directory))
    {
      ply_trace ("file '%s' is in the way", directory);
      errno = EEXIST;
      return false;
    }

  if (mkdir (directory, 0755) < 0)
    {
      char *parent_directory;
      char *last_path_component;
      bool is_created;

      is_created = errno == EEXIST;
      if (errno == ENOENT)
        {
          parent_directory = strdup (directory);
          last_path_component = strrchr (parent_directory, '/');
          *last_path_component = '\0';

          ply_trace ("parent directory '%s' doesn't exist, creating it first", parent_directory);
          if (ply_create_directory (parent_directory)
              && ((mkdir (directory, 0755) == 0) || errno == EEXIST))
            is_created = true;

          ply_save_errno ();
          free (parent_directory);
          ply_restore_errno ();

        }

      return is_created;
    }


  return true;
}

bool
ply_create_file_link (const char *source,
                      const char *destination)
{
  if (link (source, destination) < 0)
    return false;

  return true;
}

void
ply_show_new_kernel_messages (bool should_show)
{
  int type;

  if (should_show)
    type = PLY_ENABLE_CONSOLE_PRINTK;
  else
    type = PLY_DISABLE_CONSOLE_PRINTK;

  if (klogctl (type, NULL, 0) < 0)
    ply_trace ("could not toggle printk visibility: %m");
}

ply_daemon_handle_t *
ply_create_daemon (const char *pid_file)
{
  pid_t pid;
  int sender_fd, receiver_fd;
  int *handle;

  if (!ply_open_unidirectional_pipe (&sender_fd, &receiver_fd))
    return NULL;


  pid = fork ();

  if (pid < 0)
    return NULL;

  if (pid != 0)
    {
      uint8_t byte;
      close (sender_fd);

      if (!ply_read (receiver_fd, &byte, sizeof (uint8_t)))
        {
          ply_error ("could not read byte from child: %m");
          _exit (1);
        }

      if ((byte == 0) && (pid_file != NULL))
        {
          FILE *pidf;

          pidf = fopen (pid_file, "w");
          if (!pidf)
            {
              ply_error ("could not write pid file %s: %m", pid_file);
            }
          else
            {
              fprintf (pidf, "%d\n", (int)pid);
              fclose (pidf);
            }
        }

      _exit ((int) byte);
    }
  close (receiver_fd);

  handle = calloc (1, sizeof (int));
  *handle = sender_fd;

  return (ply_daemon_handle_t *) handle;
}

bool
ply_detach_daemon (ply_daemon_handle_t *handle,
                   int                  exit_code)
{
  int sender_fd;
  uint8_t byte;

  assert (handle != NULL);
  assert (exit_code >= 0 && exit_code < 256);

  sender_fd = *(int *) handle;

  byte = (uint8_t) exit_code;

  if (!ply_write (sender_fd, &byte, sizeof (uint8_t)))
    return false;

  close (sender_fd);
  free (handle);

  return true;
}


/*                    UTF-8 encoding
00000000-01111111 	00-7F 	US-ASCII (single byte)
10000000-10111111 	80-BF 	Second, third, or fourth byte of a multi-byte sequence
11000000-11011111 	C0-DF 	Start of 2-byte sequence
11100000-11101111 	E0-EF 	Start of 3-byte sequence
11110000-11110100 	F0-F4 	Start of 4-byte sequence
*/

int
ply_utf8_character_get_size (const char   *string,
                             size_t        n)
{
  int length;
  if (n < 1) return -1;
  if (string[0] == 0x00) length = 0;
  else if ((string[0] & 0x80) == 0x00) length = 1;
  else if ((string[0] & 0xE0) == 0xC0) length = 2;
  else if ((string[0] & 0xF0) == 0xE0) length = 3;
  else if ((string[0] & 0xF8) == 0xF0) length = 4;
  else return -2;
  if (length > (int) n) return -1;
  return length;
}

int
ply_utf8_string_get_length (const char   *string,
                            size_t        n)
{
  size_t count = 0;
  while (true)
    {
      int charlen = ply_utf8_character_get_size(string, n);
      if (charlen <= 0) break;
      string += charlen;
      n -= charlen;
      count++;
    }
  return count;
}

/* vim: set ts=4 sw=4 expandtab autoindent cindent cino={.5s,(0: */
