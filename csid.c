// cSID by Hermit (Mihaly Horvath), (Year 2016..2017) http://hermit.sidrip.com
// (based on jsSID but totally revorked in C to be cycle-based & oversampled)
// License: WTF - Do what the fuck you want with this code, but I please mention me as its original author.

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <SDL_config.h>
#include <SDL.h>
#include <SDL_audio.h>

typedef unsigned char byte;

//global constants and variables
#define C64_PAL_CPUCLK 985248
#define SID_CHANNEL_AMOUNT 3 
#define MAX_DATA_LEN 65536
#define PAL_FRAMERATE 49.4 //important to match, otherwise some ADSR-sensitive tunes suffer.
#define DEFAULT_SAMPLERATE 44100

int OUTPUT_SCALEDOWN = SID_CHANNEL_AMOUNT * 16 + 26; 
//raw output divided by this after multiplied by main volume, this also compensates for filter-resonance emphasis to avoid distotion

enum { GATE_BITMASK=0x01, SYNC_BITMASK=0x02, RING_BITMASK=0x04,
    TEST_BITMASK=0x08, TRI_BITMASK=0x10, SAW_BITMASK=0x20,
    PULSE_BITMASK=0x40, NOISE_BITMASK=0x80, HOLDZERO_BITMASK=0x10,
    DECAYSUSTAIN_BITMASK=0x40, ATTACK_BITMASK=0x80, LOWPASS_BITMASK=0x10,
    BANDPASS_BITMASK=0x20, HIGHPASS_BITMASK=0x40, OFF3_BITMASK=0x80 };

const byte FILTSW[9] = {1,2,4,1,2,4,1,2,4};
byte ADSRstate[9], expcnt[9], envcnt[9], sourceMSBrise[9];  
unsigned int clock_ratio=22, ratecnt[9], prevwfout[9]; 
unsigned long int phaseaccu[9], prevaccu[9], sourceMSB[3], noise_LFSR[9];
long int prevlowpass[3], prevbandpass[3];
float cutoff_ratio_8580, cutoff_ratio_6581, cutoff_bias_6581;
int SIDamount=1, SID_model[3]={8580,8580,8580}, requested_SID_model=-1, sampleratio;
byte filedata[MAX_DATA_LEN], memory[MAX_DATA_LEN], timermode[0x20], SIDtitle[0x20], SIDauthor[0x20], SIDinfo[0x20];
int subtune=0, tunelength=-1;
unsigned int initaddr, playaddr, playaddf, SID_address[3]={0xD400,0,0}; 
long int samplerate = DEFAULT_SAMPLERATE; 
int framecnt=0, frame_sampleperiod = DEFAULT_SAMPLERATE/PAL_FRAMERATE; 
//CPU (and CIA/VIC-IRQ) emulation constants and variables - avoiding internal/automatic variables to retain speed
const byte flagsw[]={0x01,0x21,0x04,0x24,0x00,0x40,0x08,0x28}, branchflag[]={0x80,0x40,0x01,0x02};
unsigned int PC=0, pPC=0, addr=0, storadd=0;
short int A=0, T=0, SP=0xFF; 
byte X=0, Y=0, IR=0, ST=0x00;  //STATUS-flags: N V - B D I Z C
char CPUtime=0, cycles=0, finished=0, dynCIA=0;

//function prototypes
void cSID_init(int samplerate);
int SID(char num, unsigned int baseaddr); void initSID();
void initCPU (unsigned int mempos);  byte CPU (); 
void init (byte subtune);  void play(void* userdata, Uint8 *stream, int len );
unsigned int combinedWF(char num, char channel, unsigned int* wfarray, int index, char differ6581);
void createCombinedWF(unsigned int* wfarray, float bitmul, float bitstrength, float treshold);



//----------------------------- MAIN thread ----------------------------

 

int main (int argc, char *argv[])
{
 int readata, strend, subtune_amount, preferred_SID_model[3]={8580.0,8580.0,8580.0}; 
 unsigned int i, datalen, offs, loadaddr;
 FILE *InputFile;
 usleep(100000); //wait a bit to avoid keypress leftover (btw this might not happen in Linux)
 //open and process the file
 if (argc<2) { printf("\nUsage: csid <inputfile> [ subtune_number [SID_modelnumber [seconds]] ]\n\n"); return 1; }
 if (argc>=3) {sscanf(argv[2],"%d",&subtune); subtune--; if (subtune<0 || subtune>63) subtune=0;} else subtune=0;
 if (argc>=4) sscanf(argv[3],"%d",&requested_SID_model);
 if (argc>=5) sscanf(argv[4],"%d",&tunelength);
 InputFile = fopen(argv[1],"rb"); if (InputFile==NULL) {printf("File not found.\n");return 1;} printf("\n"); datalen=0;
 do { readata=fgetc(InputFile); filedata[datalen++]=readata; } while (readata!=EOF && datalen<MAX_DATA_LEN); printf("\n%d bytes read (%s subtune %d)",--datalen,argv[1],subtune+1); fclose(InputFile);
 offs=filedata[7]; loadaddr=filedata[8]+filedata[9]? filedata[8]*256+filedata[9] : filedata[offs]+filedata[offs+1]*256; printf("\nOffset: $%4.4X, Loadaddress: $%4.4X \nTimermodes:", offs, loadaddr);
 for (i=0; i<32; i++) { timermode[31-i] = (filedata[0x12+(i>>3)] & (byte)pow(2,7-i%8))?1:0; printf(" %1d",timermode[31-i]); }
 for(i=0;i<MAX_DATA_LEN;i++) memory[i]=0; for (i=offs+2; i<datalen; i++) { if (loadaddr+i-(offs+2)<MAX_DATA_LEN) memory[loadaddr+i-(offs+2)]=filedata[i]; } 
 strend=1; for(i=0; i<32; i++) { if(strend!=0) strend=SIDtitle[i]=filedata[0x16+i]; else strend=SIDtitle[i]=0; }  printf("\nTitle: %s    ",SIDtitle);
 strend=1; for(i=0; i<32; i++) { if(strend!=0) strend=SIDauthor[i]=filedata[0x36+i]; else strend=SIDauthor[i]=0; }  printf("Author: %s    ",SIDauthor); 
 strend=1; for(i=0; i<32; i++) { if(strend!=0) strend=SIDinfo[i]=filedata[0x56+i]; else strend=SIDinfo[i]=0; } printf("Info: %s",SIDinfo);
 initaddr=filedata[0xA]+filedata[0xB]? filedata[0xA]*256+filedata[0xB] : loadaddr; playaddr=playaddf=filedata[0xC]*256+filedata[0xD]; printf("\nInit:$%4.4X,Play:$%4.4X, ",initaddr,playaddr);
 subtune_amount=filedata[0xF]; preferred_SID_model[0] = (filedata[0x77]&0x30)>=0x20? 8580 : 6581; printf("Subtunes:%d , preferred SID-model:%d", subtune_amount, preferred_SID_model[0]);
 preferred_SID_model[1] = (filedata[0x77]&0xC0)>=0x80 ? 8580 : 6581; preferred_SID_model[2] = (filedata[0x76]&3)>=3 ? 8580 : 6581; 
 SID_address[1] = filedata[0x7A]>=0x42 && (filedata[0x7A]<0x80 || filedata[0x7A]>=0xE0) ? 0xD000+filedata[0x7A]*16 : 0;
 SID_address[2] = filedata[0x7B]>=0x42 && (filedata[0x7B]<0x80 || filedata[0x7B]>=0xE0) ? 0xD000+filedata[0x7B]*16 : 0;
 SIDamount=1+(SID_address[1]>0)+(SID_address[2]>0); if(SIDamount>=2) printf("(SID1), %d(SID2:%4.4X)",preferred_SID_model[1],SID_address[1]); 
 if(SIDamount==3) printf(", %d(SID3:%4.4X)",preferred_SID_model[2],SID_address[2]);
 if (requested_SID_model!=-1) printf(" (requested:%d)",requested_SID_model); printf("\n");
 samplerate = DEFAULT_SAMPLERATE; sampleratio = round(C64_PAL_CPUCLK/samplerate);
 if ( SDL_Init(SDL_INIT_AUDIO) < 0 ) {fprintf(stderr, "Couldn't initialize SDL: %s\n",SDL_GetError()); return(1); }
 SDL_AudioSpec soundspec; soundspec.freq=samplerate; soundspec.channels=1; soundspec.format=AUDIO_S16; soundspec.samples=16384; soundspec.userdata=NULL; soundspec.callback=play;
 if ( SDL_OpenAudio(&soundspec, NULL) < 0 ) { fprintf(stderr, "Couldn't open audio: %s\n", SDL_GetError()); return(2); }
 for (i=0;i<SIDamount;i++) {
  if (requested_SID_model==8580 || requested_SID_model==6581) SID_model[i] = requested_SID_model;
  else SID_model[i] = preferred_SID_model[i];
 }
 if (SIDamount==2) OUTPUT_SCALEDOWN /= 0.6;
 else if (SIDamount>=3) OUTPUT_SCALEDOWN /= 0.4;
 cSID_init(samplerate); init(subtune);  
 SDL_PauseAudio(0); 
 fflush(stdin); if(tunelength!=-1) sleep(tunelength); else { printf("Press Enter to abort playback...\n"); getchar(); }
 SDL_PauseAudio(1);
 SDL_CloseAudio();
 return 0;
}


void init (byte subt)
{
 static long int timeout; subtune = subt; initCPU(initaddr); initSID(); A=subtune; memory[1]=0x37; memory[0xDC05]=0;
 for(timeout=100000;timeout>=0;timeout--) { if (CPU()) break; } 
 if (timermode[subtune] || memory[0xDC05]) { //&& playaddf {   //CIA timing
  if (!memory[0xDC05]) {memory[0xDC04]=0x24; memory[0xDC05]=0x40;} //C64 startup-default
  frame_sampleperiod = (memory[0xDC04]+memory[0xDC05]*256)/clock_ratio; }
 else frame_sampleperiod = samplerate/PAL_FRAMERATE;  //Vsync timing
 printf("Frame-sampleperiod: %d samples  (%.2fX speed)\n",frame_sampleperiod,(double)(samplerate/PAL_FRAMERATE)/frame_sampleperiod); 
 //frame_sampleperiod = (memory[0xDC05]!=0 || (!timermode[subtune] && playaddf))? samplerate/PAL_FRAMERATE : (memory[0xDC04] + memory[0xDC05]*256) / clock_ratio; 
 if(playaddf==0) { playaddr = ((memory[1]&3)<2)? memory[0xFFFE]+memory[0xFFFF]*256 : memory[0x314]+memory[0x315]*256; printf("IRQ-playaddress:%4.4X\n",playaddr); }
 else { playaddr=playaddf; if (playaddr>=0xE000 && memory[1]==0x37) memory[1]=0x35; } //player under KERNAL (Crystal Kingdom Dizzy)
 initCPU(playaddr); framecnt=1; finished=0; CPUtime=0; 
}


void play(void* userdata, Uint8 *stream, int len ) //called by SDL at samplerate pace
{ 
 static int i,j, output; static float average;
 
 for(i=0;i<len;i+=2) {
  framecnt--; if (framecnt<=0) { framecnt=frame_sampleperiod; finished=0; PC=playaddr; SP=0xFF; } // printf("%d  %f\n",framecnt,playtime); }
  average = 0.0 ;
  for (j=0; j<sampleratio; j++) {
   if (finished==0 && --cycles<=0) {
     pPC=PC; if (CPU()>=0xFE || ( (memory[1]&3)>1 && pPC<0xE000 && (PC==0xEA31 || PC==0xEA81) ) ) finished=1; //IRQ player ROM return handling
     if ( (addr==0xDC05 || addr==0xDC04) && (memory[1]&3) && timermode[subtune] ) {
      frame_sampleperiod = (memory[0xDC04] + memory[0xDC05]*256) / clock_ratio;  //dynamic CIA-setting (Galway/Rubicon workaround)
      if (!dynCIA) {dynCIA=1; printf("( Dynamic CIA settings. New frame-sampleperiod: %d samples  (%.2fX speed) )\n",frame_sampleperiod,(double)(samplerate/PAL_FRAMERATE)/frame_sampleperiod);}
     }
     if(storadd>=0xD420 && storadd<0xD800 && (memory[1]&3)) {  //CJ in the USA workaround (writing above $d420, except SID2/SID3)
      if ( !(SID_address[1]<=storadd && storadd<SID_address[1]+0x1F) && !(SID_address[2]<=storadd && storadd<SID_address[2]+0x1F) )
       memory[storadd&0xD41F]=memory[storadd]; //write to $D400..D41F if not in SID2/SID3 address-space
     }
   }
   average += SID(0,0xD400);
   if (SIDamount>=2) average += SID(1,SID_address[1]); 
   if (SIDamount==3) average += SID(2,SID_address[2]); 
  } 
  output = average / sampleratio; 
  stream[i]=output&0xFF; 
  stream[i+1]=output>>8; 
 }
 
  //mix = SID(0,0xD400); if (SID_address[1]) mix += SID(1,SID_address[1]); if(SID_address[2]) mix += SID(2,SID_address[2]);
  //return mix * volume * SIDamount_vol[SIDamount] + (Math.random()*background_noise-background_noise/2); 
}



//--------------------------------- CPU emulation -------------------------------------------



 void initCPU (unsigned int mempos) { PC=mempos; A=0; X=0; Y=0; ST=0; SP=0xFF; } 

 byte CPU () //the CPU emulation for SID/PRG playback (ToDo: CIA/VIC-IRQ/NMI/RESET vectors, BCD-mode)
 { //'IR' is the instruction-register, naming after the hardware-equivalent
  IR=memory[PC]; cycles=2; storadd=0; //'cycle': ensure smallest 6510 runtime (for implied/register instructions)
  if(IR&1) {  //nybble2:  1/5/9/D:accu.instructions, 3/7/B/F:illegal opcodes
   switch (IR&0x1F) { //addressing modes (begin with more complex cases), PC wraparound not handled inside to save codespace
    case 1: case 3: addr = memory[memory[++PC]+X] + memory[memory[PC]+X+1]*256; cycles=6; break; //(zp,x)
    case 0x11: case 0x13: addr = memory[memory[++PC]] + memory[memory[PC]+1]*256 + Y; cycles=6; break; //(zp),y
    case 0x19: case 0x1B: addr = memory[++PC] + memory[++PC]*256 + Y; cycles=5; break; //abs,y
    case 0x1D: addr = memory[++PC] + memory[++PC]*256 + X; cycles=5; break; //abs,x
    case 0xD: case 0xF: addr = memory[++PC] + memory[++PC]*256; cycles=4; break; //abs
    case 0x15: addr = memory[++PC] + X; cycles=4; break; //zp,x
    case 5: case 7: addr = memory[++PC]; cycles=3; break; //zp
    case 0x17: if ((IR&0xC0)!=0x80) { addr = memory[++PC] + X; cycles=4; } //zp,x for illegal opcodes
               else { addr = memory[++PC] + Y; cycles=4; }  break; //zp,y for LAX/SAX illegal opcodes
    case 0x1F: if ((IR&0xC0)!=0x80) { addr = memory[++PC] + memory[++PC]*256 + X; cycles=5; } //abs,x for illegal opcodes
	       else { addr = memory[++PC] + memory[++PC]*256 + Y; cycles=5; }  break; //abs,y for LAX/SAX illegal opcodes
    case 9: case 0xB: addr = ++PC; cycles=2;  //immediate
   }
   addr&=0xFFFF;
   switch (IR&0xE0) {
    case 0x60: if ((IR&0x1F)!=0xB) { if((IR&3)==3) {T=(memory[addr]>>1)+(ST&1)*128; ST&=124; ST|=(T&1); memory[addr]=T; cycles+=2;}   //ADC / RRA (ROR+ADC)
                T=A; A+=memory[addr]+(ST&1); ST&=60; ST|=(A&128)|(A>255); A&=0xFF; ST |= (!A)<<1 | ( !((T^memory[addr])&0x80) & ((T^A)&0x80) ) >> 1; }
               else { A&=memory[addr]; T+=memory[addr]+(ST&1); ST&=60; ST |= (T>255) | ( !((A^memory[addr])&0x80) & ((T^A)&0x80) ) >> 1; //V-flag set by intermediate ADC mechanism: (A&mem)+mem
                T=A; A=(A>>1)+(ST&1)*128; ST|=(A&128)|(T>127); ST|=(!A)<<1; }  break; // ARR (AND+ROR, bit0 not going to C, but C and bit7 get exchanged.)
    case 0xE0: if((IR&3)==3 && (IR&0x1F)!=0xB) {memory[addr]++;cycles+=2;}  T=A; A-=memory[addr]+!(ST&1); //SBC / ISC(ISB)=INC+SBC
               ST&=60; ST|=(A&128)|(A>=0); A&=0xFF; ST |= (!A)<<1 | ( ((T^memory[addr])&0x80) & ((T^A)&0x80) ) >> 1; break; 
    case 0xC0: if((IR&0x1F)!=0xB) { if ((IR&3)==3) {memory[addr]--; cycles+=2;}  T=A-memory[addr]; } // CMP / DCP(DEC+CMP)
               else {X=T=(A&X)-memory[addr];} /*SBX(AXS)*/  ST&=124;ST|=(!(T&0xFF))<<1|(T&128)|(T>=0);  break;  //SBX (AXS) (CMP+DEX at the same time)
    case 0x00: if ((IR&0x1F)!=0xB) { if ((IR&3)==3) {ST&=124; ST|=(memory[addr]>127); memory[addr]<<=1; cycles+=2;}  
                A|=memory[addr]; ST&=125;ST|=(!A)<<1|(A&128); } //ORA / SLO(ASO)=ASL+ORA
               else {A&=memory[addr]; ST&=124;ST|=(!A)<<1|(A&128)|(A>127);}  break; //ANC (AND+Carry=bit7)
    case 0x20: if ((IR&0x1F)!=0xB) { if ((IR&3)==3) {T=(memory[addr]<<1)+(ST&1); ST&=124; ST|=(T>255); T&=0xFF; memory[addr]=T; cycles+=2;}  
                A&=memory[addr]; ST&=125; ST|=(!A)<<1|(A&128); }  //AND / RLA (ROL+AND)
               else {A&=memory[addr]; ST&=124;ST|=(!A)<<1|(A&128)|(A>127);}  break; //ANC (AND+Carry=bit7)
    case 0x40: if ((IR&0x1F)!=0xB) { if ((IR&3)==3) {ST&=124; ST|=(memory[addr]&1); memory[addr]>>=1; cycles+=2;}
                A^=memory[addr]; ST&=125;ST|=(!A)<<1|(A&128); } //EOR / SRE(LSE)=LSR+EOR
                else {A&=memory[addr]; ST&=124; ST|=(A&1); A>>=1; A&=0xFF; ST|=(A&128)|((!A)<<1); }  break; //ALR(ASR)=(AND+LSR)
    case 0xA0: if ((IR&0x1F)!=0x1B) { A=memory[addr]; if((IR&3)==3) X=A; } //LDA / LAX (illegal, used by my 1 rasterline player) 
               else {A=X=SP=memory[addr]&SP;} /*LAS(LAR)*/  ST&=125; ST|=((!A)<<1) | (A&128); break;  // LAS (LAR)
    case 0x80: if ((IR&0x1F)==0xB) { A = X & memory[addr]; ST&=125; ST|=(A&128) | ((!A)<<1); } //XAA (TXA+AND), highly unstable on real 6502!
	       else if ((IR&0x1F)==0x1B) { SP=A&X; memory[addr]=SP&((addr>>8)+1); } //TAS(SHS) (SP=A&X, mem=S&H} - unstable on real 6502
	       else {memory[addr]=A & (((IR&3)==3)?X:0xFF); storadd=addr;}  break; //STA / SAX (at times same as AHX/SHX/SHY) (illegal) 
   }
  }
  
  else if(IR&2) {  //nybble2:  2:illegal/LDX, 6:A/X/INC/DEC, A:Accu-shift/reg.transfer/NOP, E:shift/X/INC/DEC
   switch (IR&0x1F) { //addressing modes
    case 0x1E: addr = memory[++PC] + memory[++PC]*256 + ( ((IR&0xC0)!=0x80) ? X:Y ); cycles=5; break; //abs,x / abs,y
    case 0xE: addr = memory[++PC] + memory[++PC]*256; cycles=4; break; //abs
    case 0x16: addr = memory[++PC] + ( ((IR&0xC0)!=0x80) ? X:Y ); cycles=4; break; //zp,x / zp,y
    case 6: addr = memory[++PC]; cycles=3; break; //zp
    case 2: addr = ++PC; cycles=2;  //imm.
   }  
   addr&=0xFFFF; 
   switch (IR&0xE0) {
    case 0x00: ST&=0xFE; case 0x20: if((IR&0xF)==0xA) { A=(A<<1)+(ST&1); ST&=124;ST|=(A&128)|(A>255); A&=0xFF; ST|=(!A)<<1; } //ASL/ROL (Accu)
      else { T=(memory[addr]<<1)+(ST&1); ST&=124;ST|=(T&128)|(T>255); T&=0xFF; ST|=(!T)<<1; memory[addr]=T; cycles+=2; }  break; //RMW (Read-Write-Modify)
    case 0x40: ST&=0xFE; case 0x60: if((IR&0xF)==0xA) { T=A; A=(A>>1)+(ST&1)*128; ST&=124;ST|=(A&128)|(T&1); A&=0xFF; ST|=(!A)<<1; } //LSR/ROR (Accu)
      else { T=(memory[addr]>>1)+(ST&1)*128; ST&=124;ST|=(T&128)|(memory[addr]&1); T&=0xFF; ST|=(!T)<<1; memory[addr]=T; cycles+=2; }  break; //memory (RMW)
    case 0xC0: if(IR&4) { memory[addr]--; ST&=125;ST|=(!memory[addr])<<1|(memory[addr]&128); cycles+=2; } //DEC
      else {X--; X&=0xFF; ST&=125;ST|=(!X)<<1|(X&128);}  break; //DEX
    case 0xA0: if((IR&0xF)!=0xA) X=memory[addr];  else if(IR&0x10) {X=SP;break;}  else X=A;  ST&=125;ST|=(!X)<<1|(X&128);  break; //LDX/TSX/TAX
    case 0x80: if(IR&4) {memory[addr]=X;storadd=addr;}  else if(IR&0x10) SP=X;  else {A=X; ST&=125;ST|=(!A)<<1|(A&128);}  break; //STX/TXS/TXA
    case 0xE0: if(IR&4) { memory[addr]++; ST&=125;ST|=(!memory[addr])<<1|(memory[addr]&128); cycles+=2; } //INC/NOP
   }
  }
  
  else if((IR&0xC)==8) {  //nybble2:  8:register/status
   switch (IR&0xF0) {
    case 0x60: SP++; SP&=0xFF; A=memory[0x100+SP]; ST&=125;ST|=(!A)<<1|(A&128); cycles=4; break; //PLA
    case 0xC0: Y++; Y&=0xFF; ST&=125;ST|=(!Y)<<1|(Y&128); break; //INY
    case 0xE0: X++; X&=0xFF; ST&=125;ST|=(!X)<<1|(X&128); break; //INX
    case 0x80: Y--; Y&=0xFF; ST&=125;ST|=(!Y)<<1|(Y&128); break; //DEY
    case 0x00: memory[0x100+SP]=ST; SP--; SP&=0xFF; cycles=3; break; //PHP
    case 0x20: SP++; SP&=0xFF; ST=memory[0x100+SP]; cycles=4; break; //PLP
    case 0x40: memory[0x100+SP]=A; SP--; SP&=0xFF; cycles=3; break; //PHA
    case 0x90: A=Y; ST&=125;ST|=(!A)<<1|(A&128); break; //TYA
    case 0xA0: Y=A; ST&=125;ST|=(!Y)<<1|(Y&128); break; //TAY
    default: if(flagsw[IR>>5]&0x20) ST|=(flagsw[IR>>5]&0xDF); else ST&=255-(flagsw[IR>>5]&0xDF);  //CLC/SEC/CLI/SEI/CLV/CLD/SED
   }
  }
  
  else {  //nybble2:  0: control/branch/Y/compare  4: Y/compare  C:Y/compare/JMP
   if ((IR&0x1F)==0x10) { PC++; T=memory[PC]; if(T&0x80) T-=0x100; //BPL/BMI/BVC/BVS/BCC/BCS/BNE/BEQ  relative branch 
    if(IR&0x20) {if (ST&branchflag[IR>>6]) {PC+=T;cycles=3;}} else {if (!(ST&branchflag[IR>>6])) {PC+=T;cycles=3;}}  } 
   else {  //nybble2:  0:Y/control/Y/compare  4:Y/compare  C:Y/compare/JMP
    switch (IR&0x1F) { //addressing modes
     case 0: addr = ++PC; cycles=2; break; //imm. (or abs.low for JSR/BRK)
     case 0x1C: addr = memory[++PC] + memory[++PC]*256 + X; cycles=5; break; //abs,x
     case 0xC: addr = memory[++PC] + memory[++PC]*256; cycles=4; break; //abs
     case 0x14: addr = memory[++PC] + X; cycles=4; break; //zp,x
     case 4: addr = memory[++PC]; cycles=3;  //zp
    }  
    addr&=0xFFFF;  
    switch (IR&0xE0) {
     case 0x00: memory[0x100+SP]=PC%256; SP--;SP&=0xFF; memory[0x100+SP]=PC/256;  SP--;SP&=0xFF; memory[0x100+SP]=ST; SP--;SP&=0xFF; 
       PC = memory[0xFFFE]+memory[0xFFFF]*256-1; cycles=7; break; //BRK
     case 0x20: if(IR&0xF) { ST &= 0x3D; ST |= (memory[addr]&0xC0) | ( !(A&memory[addr]) )<<1; } //BIT
      else { memory[0x100+SP]=(PC+2)%256; SP--;SP&=0xFF; memory[0x100+SP]=(PC+2)/256;  SP--;SP&=0xFF; PC=memory[addr]+memory[addr+1]*256-1; cycles=6; }  break; //JSR
     case 0x40: if(IR&0xF) { PC = addr-1; cycles=3; } //JMP
      else { if(SP>=0xFF) return 0xFE; SP++;SP&=0xFF; ST=memory[0x100+SP]; SP++;SP&=0xFF; T=memory[0x100+SP]; SP++;SP&=0xFF; PC=memory[0x100+SP]+T*256-1; cycles=6; }  break; //RTI
     case 0x60: if(IR&0xF) { PC = memory[addr]+memory[addr+1]*256-1; cycles=5; } //JMP() (indirect)
      else { if(SP>=0xFF) return 0xFF; SP++;SP&=0xFF; T=memory[0x100+SP]; SP++;SP&=0xFF; PC=memory[0x100+SP]+T*256-1; cycles=6; }  break; //RTS
     case 0xC0: T=Y-memory[addr]; ST&=124;ST|=(!(T&0xFF))<<1|(T&128)|(T>=0); break; //CPY
     case 0xE0: T=X-memory[addr]; ST&=124;ST|=(!(T&0xFF))<<1|(T&128)|(T>=0); break; //CPX
     case 0xA0: Y=memory[addr]; ST&=125;ST|=(!Y)<<1|(Y&128); break; //LDY
     case 0x80: memory[addr]=Y; storadd=addr;  //STY
    }
   }
  }

  //if(IR==0xCB) //test SBX
  // printf("PC:%4.4X IR:%2.2X, addr: %4.4X,%2.2X,  storadd: %4.4X,%2.2X,  A:%2.2X, X:%2.2X, Y:%2.2X ST:%2.2X\n",PC,IR,addr,memory[addr],storadd,memory[storadd],A,X,Y,ST);  

  PC++; //PC&=0xFFFF; 
  return 0; 
 } 



//----------------------------- SID emulation -----------------------------------------



unsigned int TriSaw_8580[4096], PulseSaw_8580[4096], PulseTriSaw_8580[4096];
int ADSRperiods[16] = {9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251};
const byte ADSR_exptable[256] = {1, 30, 30, 30, 30, 30, 30, 16, 16, 16, 16, 16, 16, 16, 16, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 4, 4, 4, 4, 4, //pos0:1  pos6:30  pos14:16  pos26:8
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, //pos54:4 //pos93:2
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };


void cSID_init(int samplerate)
{
    int i;
    clock_ratio = round(C64_PAL_CPUCLK/samplerate);
    cutoff_ratio_8580 = -2 * 3.14 * (12500.0 / 2048) / C64_PAL_CPUCLK;
    cutoff_ratio_6581 = -2 * 3.14 * (20000.0 / 2048) / C64_PAL_CPUCLK;
    cutoff_bias_6581 = 1 - exp( -2 * 3.14 * 220 / C64_PAL_CPUCLK ); //around 220Hz below treshold
 
    createCombinedWF(TriSaw_8580, 0.8, 2.4, 0.64);
    createCombinedWF(PulseSaw_8580, 1.4, 1.9, 0.68);
    createCombinedWF(PulseTriSaw_8580, 0.8, 2.5, 0.64);
    
    for(i = 0; i < 9; i++) {
        ADSRstate[i] = HOLDZERO_BITMASK; envcnt[i] = 0; ratecnt[i] = 0; 
        phaseaccu[i] = 0; prevaccu[i] = 0; expcnt[i] = 0; 
        noise_LFSR[i] = 0x7FFFF8; prevwfout[i] = 0;
    }
    for(i = 0; i < 3; i++) {
        sourceMSBrise[i] = 0; sourceMSB[i] = 0;
        prevlowpass[i] = 0; prevbandpass[i] = 0;
    }
   initSID();
}


void initSID() { 
  int i;
  for(i=0xD400;i<=0xD7FF;i++) memory[i]=0; for(i=0xDE00;i<=0xDFFF;i++) memory[i]=0;
  for(i=0;i<9;i++) {ADSRstate[i]=HOLDZERO_BITMASK; ratecnt[i]=envcnt[i]=expcnt[i]=0;} 
 }

int SID(char num, unsigned int baseaddr)
{
    //better keep these variables static so they won't slow down the routine like if they were internal automatic variables always recreated
    static byte channel, ctrl, SR, prevgate, wf, test, filterctrl_prescaler[3]; 
    static byte *sReg, *vReg;
    static unsigned int period, accuadd, pw, wfout;
    static unsigned long int MSB;
    static int nonfilt, filtin, cutoff[3], resonance[3]; //cutoff must be signed otherwise compiler may make errors in multiplications
    static long int output, filtout, ftmp;              //so if samplerate is smaller, cutoff needs to be 'long int' as its value can exceed 32768

    filtin=nonfilt=0; sReg = &memory[baseaddr]; vReg = sReg;
    for (channel = num * SID_CHANNEL_AMOUNT ; channel < (num + 1) * SID_CHANNEL_AMOUNT ; channel++, vReg += 7) {
        ctrl = vReg[4];

        //ADSR envelope generator:
        {
            SR = vReg[6];
            prevgate = (ADSRstate[channel] & GATE_BITMASK);
            if (prevgate != (ctrl & GATE_BITMASK)) { //gatebit-change?
                if (prevgate) {
                    ADSRstate[channel] &= 0xFF - (GATE_BITMASK | ATTACK_BITMASK | DECAYSUSTAIN_BITMASK);
                } //falling edge
                else {
                    ADSRstate[channel] = (GATE_BITMASK | ATTACK_BITMASK | DECAYSUSTAIN_BITMASK); //rising edge, also sets hold_zero_bit=0
                }
            }
            if (ADSRstate[channel] & ATTACK_BITMASK) period = ADSRperiods[ vReg[5] >> 4 ];
	    else if (ADSRstate[channel] & DECAYSUSTAIN_BITMASK) period = ADSRperiods[ vReg[5] & 0xF ];
	    else period = ADSRperiods[ SR & 0xF ];
            ratecnt[channel]++; ratecnt[channel]&=0x7FFF;   //can wrap around (ADSR delay-bug: short 1st frame)
            if (ratecnt[channel] == period) { //ratecounter shot (matches rateperiod) (in genuine SID ratecounter is LFSR)
                ratecnt[channel] = 0; //reset rate-counter on period-match
                if ((ADSRstate[channel] & ATTACK_BITMASK) || ++expcnt[channel] == ADSR_exptable[envcnt[channel]]) {
                    expcnt[channel] = 0; 
                    if (!(ADSRstate[channel] & HOLDZERO_BITMASK)) {
                        if (ADSRstate[channel] & ATTACK_BITMASK) {
			    envcnt[channel]++;
                            if (envcnt[channel]==0xFF) ADSRstate[channel] &= 0xFF - ATTACK_BITMASK;
                        } 
                        else if ( !(ADSRstate[channel] & DECAYSUSTAIN_BITMASK) || envcnt[channel] != (SR>>4)+(SR&0xF0) ) {
                            envcnt[channel]--; //resid adds 1 cycle delay, we omit that pipelining mechanism here
                            if (envcnt[channel]==0) ADSRstate[channel] |= HOLDZERO_BITMASK;
                        }
                    }
                }
            }
        }
        
        //WAVE generation codes (phase accumulator and waveform-selector):
        test = ctrl & TEST_BITMASK;
        wf = ctrl & 0xF0;
        accuadd = (vReg[0] + vReg[1] * 256);
        if (test || ((ctrl & SYNC_BITMASK) && sourceMSBrise[num])) {
            phaseaccu[channel] = 0;
        } else {
            phaseaccu[channel] += accuadd; phaseaccu[channel]&=0xFFFFFF;
        }
        MSB = phaseaccu[channel] & 0x800000;
        sourceMSBrise[num] = (MSB > (prevaccu[channel] & 0x800000)) ? 1 : 0;
        if (wf & NOISE_BITMASK) {
            int tmp = noise_LFSR[channel];
            if (((phaseaccu[channel] & 0x100000) != (prevaccu[channel] & 0x100000))) { 
                int step = (tmp & 0x400000) ^ ((tmp & 0x20000) << 5);
                tmp = ((tmp << 1) + (step ? 1 : test)) & 0x7FFFFF;
                noise_LFSR[channel] = tmp;
            }
            wfout = (wf & 0x70) ? 0 : ((tmp & 0x100000) >> 5) + ((tmp & 0x40000) >> 4) + ((tmp & 0x4000) >> 1) + ((tmp & 0x800) << 1) + ((tmp & 0x200) << 2) + ((tmp & 0x20) << 5) + ((tmp & 0x04) << 7) + ((tmp & 0x01) << 8);
        } else if (wf & PULSE_BITMASK) {
            pw = (vReg[2] + (vReg[3] & 0xF) * 256) * 16;
            
            int tmp = phaseaccu[channel] >> 8;
            if (wf == PULSE_BITMASK) {
                if (test || tmp>=pw) wfout = 0xFFFF;
                else {
                    wfout=0;
                }
            }
            else { //combined pulse
                wfout = (tmp >= pw || test) ? 0xFFFF : 0; 
                if (wf & TRI_BITMASK) {
                    if (wf & SAW_BITMASK) {
                        wfout = (wfout) ? combinedWF(num, channel, PulseTriSaw_8580, tmp >> 4, 1) : 0;
                    } //pulse+saw+triangle (waveform nearly identical to tri+saw)
                    else {
                        tmp = phaseaccu[channel] ^ (ctrl & RING_BITMASK ? sourceMSB[num] : 0);
                        wfout = (wfout) ? combinedWF(num, channel, PulseSaw_8580, (tmp ^ (tmp & 0x800000 ? 0xFFFFFF : 0)) >> 11, 0) : 0;
                    }
                } //pulse+triangle
                else if (wf & SAW_BITMASK) wfout = (wfout) ? combinedWF(num, channel, PulseSaw_8580, tmp >> 4, 1) : 0;
            }
        } //pulse+saw
        else if (wf & SAW_BITMASK) { 
            wfout = phaseaccu[channel] >> 8; //saw
            if (wf & TRI_BITMASK) wfout = combinedWF(num, channel, TriSaw_8580, wfout >> 4, 1); //saw+triangle
        }
        else if (wf & TRI_BITMASK) {
            int tmp = phaseaccu[channel] ^ (ctrl & RING_BITMASK ? sourceMSB[num] : 0);
            wfout = (tmp ^ (tmp & 0x800000 ? 0xFFFFFF : 0)) >> 7;
        }
        if (wf) prevwfout[channel] = wfout;
        else {
            wfout = prevwfout[channel];
        } //emulate waveform 00 floating wave-DAC
        prevaccu[channel] = phaseaccu[channel];
        sourceMSB[num] = MSB;
        if (sReg[0x17] & FILTSW[channel]) filtin += ((long int)wfout - 0x8000) * envcnt[channel] / 256;
        else if ((FILTSW[channel] != 4) || !(sReg[0x18] & OFF3_BITMASK)) 
                nonfilt += ((long int)wfout - 0x8000) * envcnt[channel] / 256;
    }
    //update readable SID1-registers (some SID tunes might use 3rd channel ENV3/OSC3 value as control)
    if(num==0, memory[1]&3) { sReg[0x1B]=wfout>>8; sReg[0x1C]=envcnt[3]; } //OSC3, ENV3 (some players rely on it) 
    
    //FILTER:
    filterctrl_prescaler[num]--;
    if (filterctrl_prescaler[num]==0)
    {  //calculate cutoff and resonance curves only at samplerate is still adequate and reduces CPU stress of frequent float calculations
     filterctrl_prescaler[num]=clock_ratio;
     cutoff[num] = 2 + sReg[0x16] * 8 + (sReg[0x15] & 7);
     if (SID_model[num] == 8580) {
         cutoff[num] = ( 1 - exp(cutoff[num] * cutoff_ratio_8580) ) * 0x10000;
         resonance[num] = ( pow(2, ((4 - (sReg[0x17] >> 4)) / 8.0)) ) * 0x100; //resonance could be taken from table as well
     } else {
         cutoff[num] = (  cutoff_bias_6581 + ( (cutoff[num] < 192) ? 0 : 1 - exp((cutoff[num]-192) * cutoff_ratio_6581) )  ) * 0x10000;
         resonance[num] = ( (sReg[0x17] > 0x5F) ? 8.0 / (sReg[0x17] >> 4) : 1.41 ) * 0x100;
     }  
    }
    filtout=0; //the filter-calculation itself can't be prescaled because sound-quality would suffer of no 'oversampling'
    ftmp = filtin + prevbandpass[num] * resonance[num] / 0x100 + prevlowpass[num];
    if (sReg[0x18] & HIGHPASS_BITMASK) filtout -= ftmp;
    ftmp = prevbandpass[num] - ftmp * cutoff[num] / 0x10000;
    prevbandpass[num] = ftmp;
    if (sReg[0x18] & BANDPASS_BITMASK) filtout -= ftmp;
    ftmp = prevlowpass[num] + ftmp * cutoff[num] / 0x10000;
    prevlowpass[num] = ftmp;
    if (sReg[0x18] & LOWPASS_BITMASK) filtout += ftmp;    

    //output stage for one SID
    output = (nonfilt+filtout) * (sReg[0x18]&0xF) / OUTPUT_SCALEDOWN;
    if (output>=32767) output=32767; else if (output<=-32768) output=-32768; //saturation logic on overload (not needed if the callback handles it)
    return (int)output; // master output
}



unsigned int combinedWF(char num, char channel, unsigned int* wfarray, int index, char differ6581)
{
    if(differ6581 && SID_model[num]==6581) index&=0x7FF; 
    return wfarray[index];
}

void createCombinedWF(unsigned int* wfarray, float bitmul, float bitstrength, float treshold)
{
    int  i,j,k;
    for (i=0; i<4096; i++) { wfarray[i]=0; for (j=0; j<12;j++) {
        float bitlevel=0; for (k=0; k<12; k++) {
            bitlevel += ( bitmul/pow(bitstrength,fabs(k-j)) ) * (((i>>k)&1)-0.5) ;
        }
        wfarray[i] += (bitlevel>=treshold)? pow(2,j) : 0; } wfarray[i]*=12;  }
}


