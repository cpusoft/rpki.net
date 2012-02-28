#!/usr/local/bin/python

"""
$Id$

Copyright (C) 2012 Internet Systems Consortium, Inc. ("ISC")

Permission to use, copy, modify, and/or distribute this software for any
purpose with or without fee is hereby granted, provided that the above
copyright notice and this permission notice appear in all copies.

THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.
"""

import urllib2
import httplib
import socket
import ssl
import urlparse
import zipfile
import sys
import os
import email.utils
import datetime
import base64
import hashlib
import subprocess
import syslog
import traceback
import ConfigParser

import transmissionrpc

cfg = ConfigParser.RawConfigParser()
cfg.read([os.path.join(dn, fn)
          for fn in ("rcynic.conf", "rpki.conf")
          for dn in ("/var/rcynic/etc", "/usr/local/etc", "/etc")])

section = "rpki-torrent"

zip_url = cfg.get(section, "zip_url")
zip_dir = cfg.get(section, "zip_dir")
zip_ta  = cfg.get(section, "zip_ta")

rcynic_prog = cfg.get(section, "rcynic_prog")
rcynic_conf = cfg.get(section, "rcynic_conf")

tr_env_vars = ("TR_APP_VERSION", "TR_TIME_LOCALTIME", "TR_TORRENT_DIR",
               "TR_TORRENT_ID", "TR_TORRENT_HASH", "TR_TORRENT_NAME")


class WrongServer(Exception):
  "Hostname not in X.509v3 subjectAltName extension."

class UnexpectedRedirect(Exception):
  "Unexpected HTTP redirect."

class WrongMode(Exception):
  "Wrong operation for mode."

class BadFormat(Exception):
  "ZIP file does not match our expectations."

class InconsistentEnvironment(Exception):
  "Environment variables received from Transmission aren't consistent."


def main():
  syslog.openlog("rpki-torrent", syslog.LOG_PID | syslog.LOG_PERROR)
  syslog.syslog("main() running")
  try:
    if all(v in os.environ for v in tr_env_vars):
      torrent_completion_main()
    elif not any(v in os.environ for v in tr_env_vars):
      cronjob_main()
    else:
      raise InconsistentEnvironment
  except Exception, e:
    for line in traceback.format_exc().splitlines():
      syslog.syslog(line)
    sys.exit(1)


def cronjob_main():
  syslog.syslog("cronjob_main() running")

  z = ZipFile(url = zip_url, dir = zip_dir, ta  = zip_ta)
  client = transmissionrpc.client.Client()

  if z.fetch():
    remove_torrents(client, z.torrent_name)
    syslog.syslog("Adding torrent")
    client.add(z.get_torrent())

  else:
    syslog.syslog("ZIP file did not change")
    run_rcynic(client, z.torrent_name)


def torrent_completion_main():
  syslog.syslog("torrent_completion_main() running")

  z = ZipFile(url = zip_url, dir = zip_dir, ta  = zip_ta)
  client = transmissionrpc.client.Client()

  torrent = client.info([int(os.getenv("TR_TORRENT_ID"))]).popitem()[1]
  if torrent.name != os.getenv("TR_TORRENT_NAME") or torrent.name != z.torrent_name:
    raise InconsistentEnvironment

  if torrent is None or torrent.status != "seeding":
    syslog.syslog("Torrent not ready for checking, how did I get here?")
    sys.exit(1)

  syslog.syslog("Checking manifest against disk")

  download_dir = client.get_session().download_dir

  manifest_from_disk = create_manifest(download_dir, z.torrent_name)
  manifest_from_zip = z.get_manifest()
  excess_files = set(manifest_from_disk) - set(manifest_from_zip)

  for fn in excess_files:
    del manifest_from_disk[fn]

  if manifest_from_disk != manifest_from_zip:
    syslog.syslog("Manifest does not match what torrent retrieved")
    sys.exit(1)

  if excess_files:
    syslog.syslog("Cleaning up excess files")
  for fn in excess_files:
    os.unlink(os.path.join(download_dir, fn))

  run_rcynic(client, z.torrent_name)


def run_rcynic(client, torrent_name):
  """
  Run rcynic and any other post-processing we might want (latter NIY).
  """
  
  syslog.syslog("Running rcynic")
  subprocess.check_call((rcynic_prog, "-c", rcynic_conf, "-u", os.path.join(client.get_session().download_dir, torrent_name)))

  # This probably should be configurable
  subprocess.check_call((sys.executable, "/var/rcynic/etc/rcynic.py", "/var/rcynic/data/rcynic.xml", "/var/rcynic/data/rcynic.html"))


# See http://www.minstrel.org.uk/papers/sftp/ for details on how to
# set up safe upload-only SFTP directories on the server.  In
# particular http://www.minstrel.org.uk/papers/sftp/builtin/ is likely
# to be the right path.


class ZipFile(object):
  """
  Augmented version of standard python zipfile.ZipFile class, with
  some extra methods and specialized capabilities.

  All methods of the standard zipfile.ZipFile class are supported, but
  the constructor arguments are different, and opening the ZIP file
  itself is deferred until a call which requires this, since the file
  may first need to be fetched via HTTPS.
  """

  def __init__(self, url, dir, ta, verbose = True, mode = "r"):
    self.url = url
    self.dir = dir
    self.ta = ta
    self.verbose = verbose
    self.mode = mode
    self.filename = os.path.join(dir, os.path.basename(url))
    self.changed = False
    self.zf = None
    self.peercert = None
    self.torrent_name, zip_ext = os.path.splitext(os.path.basename(url))
    if zip_ext != ".zip":
      raise BadFormat


  def __getattr__(self, name):
    if self.zf is None:
      self.zf = zipfile.ZipFile(self.filename, mode = self.mode,
                                compression = zipfile.ZIP_DEFLATED)
    return getattr(self.zf, name)


  def build_opener(self):
    """
    Voodoo to create a urllib2.OpenerDirector object with TLS
    certificate checking enabled and a hook to set self.peercert so
    our caller can check the subjectAltName field.

    You probably don't want to look at this if you can avoid it.
    """

    # Yes, we're constructing one-off classes.  Look away, look away.

    class HTTPSConnection(httplib.HTTPSConnection):
      zip = self
      def connect(self):
        sock = socket.create_connection((self.host, self.port), self.timeout)
        if getattr(self, "_tunnel_host", None):
          self.sock = sock
          self._tunnel()
        self.sock = ssl.wrap_socket(sock,
                                    keyfile = self.key_file,
                                    certfile = self.cert_file,
                                    cert_reqs = ssl.CERT_REQUIRED,
                                    ssl_version = ssl.PROTOCOL_TLSv1,
                                    ca_certs = self.zip.ta)
        self.zip.peercert = self.sock.getpeercert()

    class HTTPSHandler(urllib2.HTTPSHandler):
      def https_open(self, req):
        return self.do_open(HTTPSConnection, req)

    return urllib2.build_opener(HTTPSHandler)


  def check_subjectAltNames(self):
    """
    Check self.peercert against URL to make sure we were talking to
    the right HTTPS server.
    """

    hostname = urlparse.urlparse(self.url).hostname
    subjectAltNames = set(i[1]
                          for i in self.peercert.get("subjectAltName", ())
                          if i[0] == "DNS")
    if hostname not in subjectAltNames:
      raise WrongServer


  def download_file(self, r, bufsize = 4096):
    """
    Downloaded file to disk.
    """

    tempname = self.filename + ".new"
    f = open(tempname, "wb")
    n = int(r.info()["Content-Length"])
    for i in xrange(0, n - bufsize, bufsize):
      f.write(r.read(bufsize))
    f.write(r.read())
    f.close()
    mtime = email.utils.mktime_tz(email.utils.parsedate_tz(r.info()["Last-Modified"]))
    os.utime(tempname, (mtime, mtime))
    os.rename(tempname, self.filename)


  def fetch(self):
    """
    Fetch ZIP file from URL given to constructor.
    This only works in read mode, makes no sense in write mode.
    """

    if self.mode != "r":
      raise WrongMode

    headers = { "User-Agent" : "rpki-torrent" }
    try:
      headers["If-Modified-Since"] = email.utils.formatdate(
        os.path.getmtime(self.filename), False, True)
    except OSError:
      pass

    syslog.syslog("Checking %s..." % self.url)
    try:
      r = self.build_opener().open(urllib2.Request(self.url, None, headers))
      syslog.syslog("File has changed, starting download.")
      self.changed = True
    except urllib2.HTTPError, e:
      if e.code != 304:
        raise
      r = None
      syslog.syslog("No change.")

    self.check_subjectAltNames()

    if r is not None and r.geturl() != self.url:
      raise UnexpectedRedirect

    if r is not None:
      self.download_file(r)
      r.close()

    return self.changed


  def check_format(self):
    """
    Make sure that format of ZIP file matches our preconceptions: it
    should contain two files, one of which is the .torrent file, the
    other is the manifest, with names derived from the torrent name
    inferred from the URL.
    """

    if set(self.namelist()) != set((self.torrent_name + ".torrent", self.torrent_name + ".manifest")):
      raise BadFormat


  def get_torrent(self):
    """
    Extract torrent file from ZIP file, encoded in Base64 because
    that's what the transmisionrpc library says it wants.
    """

    self.check_format()
    return base64.b64encode(self.read(self.torrent_name + ".torrent"))


  def get_manifest(self):
    """
    Extract manifest from ZIP file, as a dictionary.

    For the moment we're fixing up the internal file names from the
    format that the existing shell-script prototype uses, but this
    should go away once this program both generates and checks the
    manifests.
    """

    self.check_format()
    result = {}
    for line in self.open(self.torrent_name + ".manifest"):
      h, fn = line.split()
      #
      # Fixup for earlier manifest format, this should go away
      if not fn.startswith(self.torrent_name):
        fn = os.path.normpath(os.path.join(self.torrent_name, fn))
      #
      result[fn] = h
    return result


def create_manifest(topdir, torrent_name):
  """
  Generate a manifest, expressed as a dictionary.
  """

  result = {}
  topdir = os.path.abspath(topdir)
  for dirpath, dirnames, filenames in os.walk(os.path.join(topdir, torrent_name)):
    for filename in filenames:
      filename = os.path.join(dirpath, filename)
      f = open(filename, "rb")
      result[os.path.relpath(filename, topdir)] = hashlib.sha256(f.read()).hexdigest()
      f.close()
  return result


def remove_torrents(client, name):
  """
  Remove any torrents with the given name.  In theory there should
  never be more than one, but it doesn't cost us much to check.
  """

  ids = [i for i, t in client.list().iteritems() if t.name == name]
  if ids:
    syslog.syslog("Removing torrent(s)")
    client.remove(ids)


if __name__ == "__main__":
  main()