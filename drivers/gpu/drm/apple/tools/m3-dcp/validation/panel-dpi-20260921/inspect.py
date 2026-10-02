import ctypes as C,json,os,subprocess
from pathlib import Path
class Connector(C.Structure):
 _fields_=[(n,C.c_uint32) for n in ('connector_id','encoder_id','connector_type','connector_type_id','connection','mmWidth','mmHeight')]
lib=C.CDLL('libdrm.so.2');lib.drmModeGetConnector.argtypes=[C.c_int,C.c_uint32];lib.drmModeGetConnector.restype=C.POINTER(Connector);lib.drmModeFreeConnector.argtypes=[C.POINTER(Connector)]
connectors=[]
for path in Path('/sys/class/drm').glob('card*-eDP-*'):
 fd=os.open('/dev/dri/'+path.name.split('-')[0],os.O_RDONLY)
 c=lib.drmModeGetConnector(fd,int((path/'connector_id').read_text()))
 assert c
 connectors.append({'name':path.name,'physical_mm':[c.contents.mmWidth,c.contents.mmHeight],'modes':(path/'modes').read_text().splitlines()})
 lib.drmModeFreeConnector(c);os.close(fd)
node=Path('/sys/firmware/devicetree/base')
dcp=node/(node/'aliases/dcp').read_bytes().rstrip(b'\0').decode().lstrip('/')
physical=[int.from_bytes((dcp/'panel'/k).read_bytes(),'big') for k in ('width-mm','height-mm')]
env=dict(os.environ,XDG_RUNTIME_DIR='/run/user/1000',DBUS_SESSION_BUS_ADDRESS='unix:path=/run/user/1000/bus')
q=subprocess.check_output(['runuser','-u','eryk','--','busctl','--user','--json=short','call','org.kde.KWin','/KWin','org.kde.KWin','supportInformation'],env=env,text=True,timeout=5)
info=json.loads(q)['data'][0]
print(json.dumps({'boot_id':Path('/proc/sys/kernel/random/boot_id').read_text().strip(),'device_tree_mm':physical,'drm_connectors':connectors,'kwin_support':info,'kwin_config':json.loads(Path('/home/eryk/.config/kwinoutputconfig.json').read_text())},indent=2))
