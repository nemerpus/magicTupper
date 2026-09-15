package com.magictupper.remote;
import android.content.*; import android.hardware.usb.*; import java.io.*; import java.nio.*; import java.security.*; import java.util.*;
public final class RcmInjector {
 public static final int VID=0x0955,PID=0x7321,MAX=0x30298; private static final int RCM=0x40010000,INTER=0x4001F000,LOAD=0x40020000;
 static {System.loadLibrary("magictupper_rcm");} private RcmInjector(){}
 public interface Log {void line(String s);} public static native int nativeTriggerExploit(int fd,int length);
 public static UsbDevice find(UsbManager m){for(UsbDevice d:m.getDeviceList().values())if(d.getVendorId()==VID&&d.getProductId()==PID)return d;return null;}
 public static void inject(Context ctx,UsbDevice dev,byte[] target,Log log)throws Exception{
  if(target==null||target.length<512)throw new IOException("Payload vacío o demasiado pequeño"); if(target.length>130000)throw new IOException("Payload demasiado grande para RCM: "+target.length);
  UsbManager um=(UsbManager)ctx.getSystemService(Context.USB_SERVICE); UsbDeviceConnection c=um.openDevice(dev); if(c==null)throw new IOException("No se pudo abrir APX/RCM");
  UsbInterface it=dev.getInterface(0); if(!c.claimInterface(it,true)){c.close();throw new IOException("No se pudo reclamar interfaz USB");}
  UsbEndpoint in=null,out=null; for(int i=0;i<it.getEndpointCount();i++){UsbEndpoint e=it.getEndpoint(i);if(e.getType()==UsbConstants.USB_ENDPOINT_XFER_BULK){if(e.getDirection()==UsbConstants.USB_DIR_IN)in=e;else out=e;}}
  if(in==null||out==null)throw new IOException("Endpoints RCM no encontrados"); byte[] id=new byte[16]; if(c.bulkTransfer(in,id,16,1500)!=16)throw new IOException("No se pudo leer Device ID"); log.line("RCM detectado · ID "+hex(id));
  byte[] inter=readAll(ctx.getAssets().open("intermezzo.bin")); ByteBuffer b=ByteBuffer.allocate(MAX).order(ByteOrder.LITTLE_ENDIAN); b.putInt(MAX); b.put(new byte[676]); for(int a=RCM;a<INTER;a+=4)b.putInt(INTER); b.put(inter); b.put(new byte[LOAD-INTER-inter.length]); b.put(target); int len=b.position(); b.position(0);
  boolean low=true; int sent=0; byte[] chunk=new byte[0x1000]; while(sent<len||low){b.get(chunk);int n=c.bulkTransfer(out,chunk,chunk.length,1500);if(n!=chunk.length)throw new IOException("Fallo USB enviando payload en 0x"+Integer.toHexString(sent));sent+=0x1000;low=!low;} log.line("Payload enviado · "+sent+" bytes");
  int rc=nativeTriggerExploit(c.getFileDescriptor(),0x7000); log.line(rc==0?"Inyección completada; la Switch debería arrancar.":"Trigger RCM devolvió "+rc); try{c.releaseInterface(it);}catch(Exception ignored){} c.close(); if(rc!=0)throw new IOException("No se pudo disparar Fusée Gelée (rc="+rc+")");
 }
 static byte[] readAll(InputStream in)throws IOException{ByteArrayOutputStream o=new ByteArrayOutputStream();byte[]x=new byte[8192];for(int n;(n=in.read(x))>0;)o.write(x,0,n);in.close();return o.toByteArray();}
 static String hex(byte[]b){StringBuilder s=new StringBuilder();for(byte x:b)s.append(String.format(Locale.US,"%02X",x));return s.toString();}
}
