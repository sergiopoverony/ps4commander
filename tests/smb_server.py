#!/usr/bin/env python3
"""Тестовый SMB-сервер (impacket): python3 tests/smb_server.py <каталог> [порт]. Логин test / пароль secret, шара DATA."""
import sys, os, logging
from impacket import smbserver
d = os.path.abspath(sys.argv[1]); port = int(sys.argv[2]) if len(sys.argv) > 2 else 445
logging.getLogger().setLevel(logging.CRITICAL)
srv = smbserver.SimpleSMBServer(listenAddress='127.0.0.1', listenPort=port)
srv.addShare('DATA', d, '')
srv.setSMB2Support(True)
srv.addCredential('test', 0, smbserver.ntlm.LMOWFv1('secret', '') .hex() if False else '', smbserver.ntlm.NTOWFv1('secret').hex())
srv.setLogFile('')
srv.start()
