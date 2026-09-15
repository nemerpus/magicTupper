package com.magictupper.remote;
final class PadState {
    static final int A=1<<0,B=1<<1,X=1<<2,Y=1<<3,L=1<<4,R=1<<5,ZL=1<<6,ZR=1<<7,MINUS=1<<8,PLUS=1<<9,L3=1<<10,R3=1<<11,UP=1<<12,DOWN=1<<13,LEFT=1<<14,RIGHT=1<<15,HOME=1<<16,CAPTURE=1<<17;
    volatile int buttons; volatile short lx,ly,rx,ry; volatile int lt,rt;
    void button(int bit, boolean down){ if(down) buttons|=bit; else buttons&=~bit; }
    static short axis(float v){ v=Math.max(-1f,Math.min(1f,v)); return (short)(v*32767f); }
}
