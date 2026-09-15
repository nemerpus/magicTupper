package com.magictupper.remote;
import android.media.*; import android.view.Surface; import java.nio.ByteBuffer;
final class H264SurfaceDecoder implements AutoCloseable {
 private MediaCodec codec; private boolean started;
 H264SurfaceDecoder(Surface surface) throws Exception { codec=MediaCodec.createDecoderByType("video/avc"); MediaFormat f=MediaFormat.createVideoFormat("video/avc",1280,720); f.setInteger(MediaFormat.KEY_LOW_LATENCY,1); codec.configure(f,surface,null,0); codec.start(); started=true; }
 synchronized void queue(byte[] au){ if(!started)return; try{ int i=codec.dequeueInputBuffer(0); if(i>=0){ByteBuffer b=codec.getInputBuffer(i); if(b!=null){b.clear(); if(au.length<=b.remaining()){b.put(au); codec.queueInputBuffer(i,0,au.length,System.nanoTime()/1000,0);} }} MediaCodec.BufferInfo info=new MediaCodec.BufferInfo(); int o; while((o=codec.dequeueOutputBuffer(info,0))>=0) codec.releaseOutputBuffer(o,true); }catch(Exception ignored){} }
 public synchronized void close(){started=false; try{codec.stop();}catch(Exception ignored){} try{codec.release();}catch(Exception ignored){} }
}
