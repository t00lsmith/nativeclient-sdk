#!/usr/bin/python
# Copyright 2010, Google Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met:
#
#     * Redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer.
#     * Redistributions in binary form must reproduce the above
# copyright notice, this list of conditions and the following disclaimer
# in the documentation and/or other materials provided with the
# distribution.
#     * Neither the name of Google Inc. nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

"""Assemble the final installer for each platform.

At this time this is just a tarball.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

EXCLUDE_DIRS = ['.svn', '.download', 'scons-out', 'packages']

# Return True if |file| should be excluded from the tarball.
def ExcludeFile(file):
  return (file.startswith('.DS_Store') or
          re.search('^\._', file) or
          file == 'DEPS')


# Note that this function has to be run from within a subversion working copy.
def SVNRevision():
  p = subprocess.Popen(['svn', 'info'],
                       stdout=subprocess.PIPE)
  svn_info = p.communicate()[0]
  m = re.search('Revision: ([0-9]+)', svn_info)
  if m:
    return int(m.group(1))
  else:
    return 0


def VersionString():
  rev = SVNRevision()
  build_number = os.environ.get('BUILD_NUMBER', '0')
  return 'native_client_sdk_0_1_%d_%s' % (rev, build_number)


def main(argv):
  # Cache the current location so we can return here before removing the
  # temporary dirs.
  home_dir = os.path.realpath(os.curdir)

  os.chdir('src')

  version_dir = VersionString()

  # Create a temporary directory using the version string, then move the
  # contents of src to that directory, clean the directory of unwanted
  # stuff and finally tar it all up using the platform's tar.  There seems to
  # be a problem with python's tarfile module and symlinks.
  temp_dir = tempfile.mkdtemp();
  installer_dir = os.path.join(temp_dir, version_dir)
  try:
    os.makedirs(installer_dir)
  except OSError:
    pass

  # Decide environment to run in per platform.
  # This adds the assumption that cygwin is installed in the default location
  # when cooking the sdk for windows.
  env = os.environ.copy()
  if sys.platform == 'win32':
    env['PATH'] = r'c:\cygwin\bin;' + env['PATH']

  # Use native tar to copy the SDK into the build location; this preserves
  # symlinks.
  tar_src_dir = os.path.realpath(os.curdir)
  tar_cf = subprocess.Popen(['tar', 'cf', '-', '.'],
                            cwd=tar_src_dir, env=env,
                            stdout=subprocess.PIPE)
  tar_xf = subprocess.Popen(['tar', 'xf', '-'],
                            cwd=installer_dir, env=env,
                            stdin=tar_cf.stdout)
  tar_copy_err = tar_xf.communicate()[1]

  # Clean out the cruft.
  os.chdir(installer_dir)
  # This loop prunes the result of os.walk() at each excluded dir, so that it
  # doesn't descend into the excluded dir.
  rm_dirs = []
  for root, dirs, files in os.walk('.'):
    for excl in EXCLUDE_DIRS:
      if excl in dirs:
        dirs.remove(excl)
        rm_dirs.append(os.path.realpath(os.path.join(root, excl)))
    for rm_dir in rm_dirs:
      try:
        shutil.rmtree(rm_dir);
      except OSError:
        pass
    rm_files = [os.path.realpath(os.path.join(root, f))
        for f in files if ExcludeFile(f)]
    for rm_file in rm_files:
      try:
        os.unlink(rm_file);
      except OSError:
        pass

  # Now that the SDK directory is copied and cleaned out, tar it all up using
  # the native platform tar.
  os.chdir(temp_dir)
  archive = os.path.join(home_dir, 'nacl-sdk.tgz')
  tarball = subprocess.Popen(['tar', 'czf', archive, version_dir], env=env)
  tarball_err = tarball.communicate()[1]

  # Clean up.
  os.chdir(home_dir)
  shutil.rmtree(temp_dir)


if __name__ == '__main__':
  main(sys.argv[1:])
