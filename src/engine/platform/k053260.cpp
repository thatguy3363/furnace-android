/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2023 tildearrow and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "k053260.h"
#include "../engine.h"
#include "../../ta-log.h"
#include <math.h>

#define rWrite(a,v) {if(!skipRegisterWrites) {k053260.write(a,v); regPool[a]=v; if(dumpWrites) addWrite(a,v);}}

#define CHIP_DIVIDER 64
#define TICK_DIVIDER 4

const char* regCheatSheetK053260[]={
  "FreqL", "0",
  "FreqH", "1",
  "LengthL", "2",
  "LengthH", "3",
  "StartL", "4",
  "StartM", "5",
  "StartH", "6",
  "Volume", "7",
  NULL
};

const char** DivPlatformK053260::getRegisterSheet() {
  return regCheatSheetK053260;
}

inline void DivPlatformK053260::chWrite(unsigned char ch, unsigned int addr, unsigned char val) {
  if (!skipRegisterWrites) {
    rWrite(8+((ch<<3)|(addr&7)),val);
  }
}

// TODO: this code is weird
//       make sure newDispatch didn't break it up
void DivPlatformK053260::acquire(short** buf, size_t len) {
  for (int i=0; i<len; i++) {
    k053260.tick(TICK_DIVIDER);
    int lout=(k053260.output(0)); // scale to 16 bit
    int rout=(k053260.output(1)); // scale to 16 bit
    if (lout>32767) lout=32767;
    if (lout<-32768) lout=-32768;
    if (rout>32767) rout=32767;
    if (rout<-32768) rout=-32768;
    buf[0][i]=lout;
    buf[1][i]=rout;

    for (int i=0; i<4; i++) {
      oscBuf[i]->data[oscBuf[i]->needle++]=(k053260.voice_out(i,0)+k053260.voice_out(i,1))>>1;
    }
  }
}

void DivPlatformK053260::tick(bool sysTick) {
  unsigned char panMask=0;
  for (int i=0; i<4; i++) {
    chan[i].std.next();
    if (chan[i].std.vol.had) {
      chan[i].outVol=((chan[i].vol&0x7f)*MIN(chan[i].macroVolMul,chan[i].std.vol.val))/chan[i].macroVolMul;
      chWrite(i,7,chan[i].outVol);
    }
    if (NEW_ARP_STRAT) {
      chan[i].handleArp();
    } else if (chan[i].std.arp.had) {
      if (!chan[i].inPorta) {
        chan[i].baseFreq=NOTE_PERIODIC(parent->calcArp(chan[i].note,chan[i].std.arp.val));
      }
      chan[i].freqChanged=true;
    }
    if (chan[i].std.pitch.had) {
      if (chan[i].std.pitch.mode) {
        chan[i].pitch2+=chan[i].std.pitch.val;
        CLAMP_VAR(chan[i].pitch2,-32768,32767);
      } else {
        chan[i].pitch2=chan[i].std.pitch.val;
      }
      chan[i].freqChanged=true;
    }
    if (chan[i].std.panL.had) { // panning
      chan[i].panning=4+chan[i].std.panL.val;
      if (!isMuted[i]) {
        panMask|=1<<i;
      }
    }
    if (chan[i].std.phaseReset.had) {
      if (chan[i].std.phaseReset.val==1 && chan[i].active) {
        chan[i].audPos=0;
        chan[i].setPos=true;
      }
    }
    if (chan[i].setPos) {
      // force keyon
      chan[i].keyOn=true;
      chan[i].setPos=false;
    } else {
      chan[i].audPos=0;
    }
    if (chan[i].freqChanged || chan[i].keyOn || chan[i].keyOff) {
      unsigned char keyon=regPool[0x28]|(1<<i);
      unsigned char keyoff=keyon&~(1<<i);
      unsigned char loopon=regPool[0x2a]|(1<<i);
      unsigned char loopoff=loopon&~(1<<i);
      double off=1.0;
      int sample=chan[i].sample;
      if (sample>=0 && sample<parent->song.sampleLen) {
        DivSample* s=parent->getSample(sample);
        if (s->centerRate<1) {
          off=1.0;
        } else {
          off=8363.0/s->centerRate;
        }
      }
      DivSample* s=parent->getSample(chan[i].sample);
      chan[i].freq=0x1000-(int)(off*parent->calcFreq(chan[i].baseFreq,chan[i].pitch,chan[i].fixedArp?chan[i].baseNoteOverride:chan[i].arpOff,chan[i].fixedArp,true,0,chan[i].pitch2,chipClock,CHIP_DIVIDER));
      if (chan[i].freq>4095) chan[i].freq=4095;
      if (chan[i].freq<0) chan[i].freq=0;
      if (chan[i].keyOn) {
        unsigned int start=0;
        unsigned int length=0;
        if (chan[i].sample>=0 && chan[i].sample<parent->song.sampleLen) {
          start=sampleOffK053260[chan[i].sample];
          length=start+s->length8;
        }
        if (chan[i].audPos>0) {
          start=start+MIN(chan[i].audPos,s->length8);
        }
        start=MIN(start,getSampleMemCapacity()-31);
        length=MIN(length,getSampleMemCapacity()-31);
        rWrite(0x28,keyoff); // force keyoff first
        rWrite(0x2a,loopoff);
        chWrite(i,2,length&0xff);
        chWrite(i,3,length>>8);
        chWrite(i,4,start&0xff);
        chWrite(i,5,start>>8);
        chWrite(i,6,start>>16);
        if (!chan[i].std.vol.had) {
          chan[i].outVol=chan[i].vol;
          chWrite(i,7,chan[i].outVol);
        }
        rWrite(0x28,keyon);
        if (s->isLoopable()) {
          rWrite(0x2a,loopon);
        }
        chan[i].keyOn=false;
      }
      if (chan[i].keyOff) {
        rWrite(0x28,keyoff);
        rWrite(0x2a,loopoff);
        chan[i].keyOff=false;
      }
      if (chan[i].freqChanged) {
        chWrite(i,0,chan[i].freq&0xff);
        chWrite(i,1,chan[i].freq>>8);
        chan[i].freqChanged=false;
      }
    }
  }
  if (panMask) {
    updatePanning(panMask);
  }
}

void DivPlatformK053260::updatePanning(unsigned char mask) {
  if (mask&3) {
    rWrite(0x2c,
      (isMuted[0]?0:chan[0].panning)|
      (isMuted[1]?0:chan[1].panning<<3));
  }
  if (mask&0xc) {
    rWrite(0x2d,
      (isMuted[2]?0:chan[2].panning)|
      (isMuted[3]?0:chan[3].panning<<3));
  }
}

int DivPlatformK053260::dispatch(DivCommand c) {
  switch (c.cmd) {
    case DIV_CMD_NOTE_ON: {
      DivInstrument* ins=parent->getIns(chan[c.chan].ins,DIV_INS_AMIGA);
      chan[c.chan].macroVolMul=ins->type==DIV_INS_AMIGA?64:127;
      if (c.value!=DIV_NOTE_NULL) chan[c.chan].sample=ins->amiga.getSample(c.value);
      if (c.value!=DIV_NOTE_NULL) {
        chan[c.chan].baseFreq=NOTE_PERIODIC(c.value);
      }
      if (chan[c.chan].sample<0 || chan[c.chan].sample>=parent->song.sampleLen) {
        chan[c.chan].sample=-1;
      }
      if (c.value!=DIV_NOTE_NULL) {
        chan[c.chan].freqChanged=true;
        chan[c.chan].note=c.value;
      }
      chan[c.chan].active=true;
      chan[c.chan].keyOn=true;
      chan[c.chan].macroInit(ins);
      if (!parent->song.brokenOutVol && !chan[c.chan].std.vol.will) {
        chan[c.chan].outVol=chan[c.chan].vol;
      }
      break;
    }
    case DIV_CMD_NOTE_OFF:
      chan[c.chan].sample=-1;
      chan[c.chan].active=false;
      chan[c.chan].keyOff=true;
      chan[c.chan].macroInit(NULL);
      break;
    case DIV_CMD_NOTE_OFF_ENV:
    case DIV_CMD_ENV_RELEASE:
      chan[c.chan].std.release();
      break;
    case DIV_CMD_INSTRUMENT:
      if (chan[c.chan].ins!=c.value || c.value2==1) {
        chan[c.chan].ins=c.value;
      }
      break;
    case DIV_CMD_VOLUME:
      if (chan[c.chan].vol!=c.value) {
        chan[c.chan].vol=c.value;
        if (!chan[c.chan].std.vol.has) {
          chan[c.chan].outVol=c.value;
          chWrite(c.chan,7,chan[c.chan].outVol);
        }
      }
      break;
    case DIV_CMD_GET_VOLUME:
      if (chan[c.chan].std.vol.has) {
        return chan[c.chan].vol;
      }
      return chan[c.chan].outVol;
      break;
    case DIV_CMD_PANNING:
      chan[c.chan].panning=MIN(parent->convertPanSplitToLinearLR(c.value,c.value2,7)+1,7);
      if (!isMuted[c.chan]) {
        updatePanning(1<<c.chan);
      }
      break;
    case DIV_CMD_PITCH:
      chan[c.chan].pitch=c.value;
      chan[c.chan].freqChanged=true;
      break;
    case DIV_CMD_NOTE_PORTA: {
      int destFreq=NOTE_PERIODIC(c.value2);
      bool return2=false;
      if (destFreq>chan[c.chan].baseFreq) {
        chan[c.chan].baseFreq+=c.value;
        if (chan[c.chan].baseFreq>=destFreq) {
          chan[c.chan].baseFreq=destFreq;
          return2=true;
        }
      } else {
        chan[c.chan].baseFreq-=c.value;
        if (chan[c.chan].baseFreq<=destFreq) {
          chan[c.chan].baseFreq=destFreq;
          return2=true;
        }
      }
      chan[c.chan].freqChanged=true;
      if (return2) {
        chan[c.chan].inPorta=false;
        return 2;
      }
      break;
    }
    case DIV_CMD_LEGATO: {
      chan[c.chan].baseFreq=NOTE_PERIODIC(c.value+((HACKY_LEGATO_MESS)?(chan[c.chan].std.arp.val-12):(0)));
      chan[c.chan].freqChanged=true;
      chan[c.chan].note=c.value;
      break;
    }
    case DIV_CMD_PRE_PORTA:
      if (chan[c.chan].active && c.value2) {
        if (parent->song.resetMacroOnPorta) chan[c.chan].macroInit(parent->getIns(chan[c.chan].ins,DIV_INS_AMIGA));
      }
      if (!chan[c.chan].inPorta && c.value && !parent->song.brokenPortaArp && chan[c.chan].std.arp.will && !NEW_ARP_STRAT) chan[c.chan].baseFreq=NOTE_PERIODIC(chan[c.chan].note);
      chan[c.chan].inPorta=c.value;
      break;
    case DIV_CMD_SAMPLE_POS:
      chan[c.chan].audPos=c.value;
      chan[c.chan].setPos=true;
      break;
    case DIV_CMD_GET_VOLMAX:
      return 127;
      break;
    case DIV_CMD_MACRO_OFF:
      chan[c.chan].std.mask(c.value,true);
      break;
    case DIV_CMD_MACRO_ON:
      chan[c.chan].std.mask(c.value,false);
      break;
    case DIV_ALWAYS_SET_VOLUME:
      return 1;
      break;
    default:
      break;
  }
  return 1;
}

void DivPlatformK053260::muteChannel(int ch, bool mute) {
  isMuted[ch]=mute;
  updatePanning(1<<ch);
}

void DivPlatformK053260::forceIns() {
  for (int i=0; i<4; i++) {
    chan[i].insChanged=true;
    chan[i].freqChanged=true;
    chan[i].sample=-1;
    chWrite(i,1,isMuted[i]?0:chan[i].panning);
  }
}

void* DivPlatformK053260::getChanState(int ch) {
  return &chan[ch];
}

DivMacroInt* DivPlatformK053260::getChanMacroInt(int ch) {
  return &chan[ch].std;
}

DivDispatchOscBuffer* DivPlatformK053260::getOscBuffer(int ch) {
  return oscBuf[ch];
}

void DivPlatformK053260::reset() {
  memset(regPool,0,48);
  k053260.reset();
  rWrite(0x28,0); // keyoff all channels
  for (int i=0; i<4; i++) {
    chan[i]=DivPlatformK053260::Channel();
    chan[i].std.setEngine(parent);
  }
  updatePanning(0xf);
  rWrite(0x2f,2); // sound enable
}

int DivPlatformK053260::getOutputCount() {
  return 2;
}

void DivPlatformK053260::notifyInsChange(int ins) {
  for (int i=0; i<4; i++) {
    if (chan[i].ins==ins) {
      chan[i].insChanged=true;
    }
  }
}

void DivPlatformK053260::notifyWaveChange(int wave) {
  // TODO when wavetables are added
  // TODO they probably won't be added unless the samples reside in RAM
}

void DivPlatformK053260::notifyInsDeletion(void* ins) {
  for (int i=0; i<4; i++) {
    chan[i].std.notifyInsDeletion((DivInstrument*)ins);
  }
}

void DivPlatformK053260::setFlags(const DivConfig& flags) {
  switch (flags.getInt("clockSel",0)) {
    case 1: chipClock=4000000; break;
    default: chipClock=COLOR_NTSC; break;
  }
  CHECK_CUSTOM_CLOCK;
  rate=chipClock/TICK_DIVIDER;
  for (int i=0; i<4; i++) {
    oscBuf[i]->rate=rate;
  }
}

void DivPlatformK053260::poke(unsigned int addr, unsigned short val) {
  rWrite(addr&0x0f,val);
}

void DivPlatformK053260::poke(std::vector<DivRegWrite>& wlist) {
  for (DivRegWrite& i: wlist) rWrite(i.addr&0x0f,i.val);
}

unsigned char* DivPlatformK053260::getRegisterPool() {
  return regPool;
}

int DivPlatformK053260::getRegisterPoolSize() {
  return 48;
}

const void* DivPlatformK053260::getSampleMem(int index) {
  return index == 0 ? sampleMem : NULL;
}

size_t DivPlatformK053260::getSampleMemCapacity(int index) {
  return index == 0 ? 2097152 : 0;
}

size_t DivPlatformK053260::getSampleMemUsage(int index) {
  return index == 0 ? sampleMemLen : 0;
}

bool DivPlatformK053260::isSampleLoaded(int index, int sample) {
  if (index!=0) return false;
  if (sample<0 || sample>255) return false;
  return sampleLoaded[sample];
}

void DivPlatformK053260::renderSamples(int sysID) {
  memset(sampleMem,0,getSampleMemCapacity());
  memset(sampleOffK053260,0,256*sizeof(unsigned int));
  memset(sampleLoaded,0,256*sizeof(bool));

  size_t memPos=1; // for avoid silence
  for (int i=0; i<parent->song.sampleLen; i++) {
    DivSample* s=parent->song.sample[i];
    if (!s->renderOn[0][sysID]) {
      sampleOffK053260[i]=0;
      continue;
    }

    int length=MIN(65535,s->getLoopEndPosition(DIV_SAMPLE_DEPTH_8BIT));
    int actualLength=MIN((int)(getSampleMemCapacity()-memPos),length);
    if (actualLength>0) {
      sampleOffK053260[i]=memPos-1;
      for (int j=0; j<actualLength; j++) {
        sampleMem[memPos++]=s->data8[j];
      }
    }
    if (actualLength<length) {
      logW("out of K053260 PCM memory for sample %d!",i);
      break;
    }
    sampleLoaded[i]=true;
  }
  sampleMemLen=memPos;
}

int DivPlatformK053260::init(DivEngine* p, int channels, int sugRate, const DivConfig& flags) {
  parent=p;
  dumpWrites=false;
  skipRegisterWrites=false;

  for (int i=0; i<4; i++) {
    isMuted[i]=false;
    oscBuf[i]=new DivDispatchOscBuffer;
  }
  sampleMem=new unsigned char[getSampleMemCapacity()];
  sampleMemLen=0;
  setFlags(flags);
  reset();
  
  return 4;
}

void DivPlatformK053260::quit() {
  delete[] sampleMem;
  for (int i=0; i<4; i++) {
    delete oscBuf[i];
  }
}
