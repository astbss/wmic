###########################################################################
#
# This program is part of Zenoss Core, an open source monitoring platform.
# Copyright (C) 2008-2010, Zenoss Inc.
#
# This program is free software; you can redistribute it and/or modify it
# under the terms of the GNU General Public License version 2, or (at your
# option) any later version, as published by the Free Software Foundation.
#
# For complete information please visit: http://www.zenoss.com/oss/
#
###########################################################################

from ctypes import *

from pysamba.twisted.reactor import reactor, eventContext
from pysamba.library import *
from pysamba.rpc.Rpc import Rpc
from pysamba.twisted.callback import WMIFailure
import sys

import pysamba.wbem.Query

from pysamba.twisted.callback import WMIFailure
import logging
log = logging.getLogger('p.t.connect')

import Globals
from Products.ZenUtils.Driver import drive, driveLater

from twisted.internet import defer

def doOneDevice(creds, hostname):
    def inner(driver):
        while 1:
            try:
                q = Rpc()
                yield q.connect(hostname, creds, 'winreg')
                driver.next()
                q.close()
            except Exception, ex:
                log.exception(ex)
    return drive(inner)

        
def main():
    logging.basicConfig()
    log = logging.getLogger()
    log.setLevel(10)
    DEBUGLEVEL.value = 99

    creds = sys.argv[1]
    hosts = sys.argv[2:]
    defs = []
    for host in hosts:
        defs.append(doOneDevice(creds, host))
    d = defer.DeferredList(defs)
    d.addBoth(lambda x : reactor.stop())
    reactor.run()

sys.exit(main())
