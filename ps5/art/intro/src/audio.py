# Crux2 - the startup video (ps5/art/intro, README.md there): its sound, synthesized, timed to main.js's
# timeline (T). Made from OGBox's audio.py (OGBox by Press5elect), on Crux2's shorter timeline.
# SPDX-FileCopyrightText: 2026 Press5elect
# SPDX-License-Identifier: GPL-3.0-or-later
# Additional term (GPL-3.0 section 7(b)): keep the attribution "Crux2 by Press5elect"; see NOTICE.
import numpy as np, wave
import os
CHORD_GAIN=float(os.environ.get('CHORD_GAIN','4.5'))   # the old video's chord, about 4 dB over the rest
SR=48000; DUR=7.0; N=int(SR*DUR)
T=dict(hb1=0.45,hb2=1.0,i1=1.55,i2=2.15,i3=2.7,burst=3.4,text=4.3,fade=6.4,end=7.0)   # main.js's T
rng=np.random.default_rng(15)
L=np.zeros(N); R=np.zeros(N)            # dry
WL=np.zeros(N); WR=np.zeros(N)          # reverb send
def add(sig,t0,pan=0.0,gain=1.0,wet=0.0):
    i=int(t0*SR)
    if i>=N: return
    if i<0: sig=sig[-i:]; i=0
    n=min(len(sig),N-i); s=sig[:n]*gain
    gl=np.cos((pan+1)*np.pi/4); gr=np.sin((pan+1)*np.pi/4)
    L[i:i+n]+=s*gl; R[i:i+n]+=s*gr
    if wet: WL[i:i+n]+=s*gl*wet; WR[i:i+n]+=s*gr*wet
def tt(d): return np.arange(int(d*SR))/SR
def noise_band(d,lo,hi,soft=0.25):
    n=int(d*SR); x=rng.standard_normal(n); X=np.fft.rfft(x); f=np.fft.rfftfreq(n,1/SR)
    m=1/(1+(lo/np.maximum(f,1e-3))**(2/soft*0.5))*1/(1+(f/hi)**(2/soft*0.5))
    y=np.fft.irfft(X*m,n); return y/ (np.abs(y).max()+1e-9)
def sweep(d,f0,f1,tau):
    t=tt(d); return np.sin(2*np.pi*(f1*t+(f0-f1)*tau*(1-np.exp(-t/tau))))
def kick(d,f0,f1,tau,dec,drive=1.0):
    t=tt(d); return np.tanh(sweep(d,f0,f1,tau)*np.exp(-t/dec)*drive)/np.tanh(drive)
def ping(freq,dec,d=None):
    d=d or dec*6; t=tt(d); return np.sin(2*np.pi*freq*t+rng.uniform(0,6.28))*np.exp(-t/dec)*np.minimum(t/0.001,1)
def fade(sig,a=0.005,r=0.02):
    n=len(sig); e=np.ones(n); na=int(a*SR); nr=int(r*SR)
    if na: e[:na]=np.linspace(0,1,na)
    if nr: e[-nr:]*=np.linspace(1,0,nr)
    return sig*e

# ---- bed: a low drone that grows until the burst
t=np.arange(N)/SR
bed=(np.sin(2*np.pi*36.7*t)+0.6*np.sin(2*np.pi*55.1*t+1)+0.35*np.sin(2*np.pi*73.6*t+2))
env=np.clip((t-0.1)/3.6,0,1)**1.6*(t<T['burst']-0.05)
add(bed*env,0,0,0.11)
rum=noise_band(DUR,40,180)*env
add(rum,0,0,0.07)

# ---- heartbeats
for t0,g in ((T['hb1'],0.5),(T['hb2'],0.8)):
    add(kick(0.5,70,36,0.05,0.13),t0+0.02,0,g*0.75,0.12)
    add(kick(0.5,62,33,0.05,0.16),t0+0.25,0,g*0.6,0.12)

# ---- the three blows
def cracks(t0,span,n,g):
    for _ in range(n):
        dt=rng.exponential(span/3.0)
        if dt>span*2: continue
        d=rng.uniform(0.002,0.012); c=noise_band(d+0.004,rng.uniform(1800,4000),rng.uniform(6000,14000))*np.exp(-tt(d+0.004)/(d/3))
        add(c,t0+dt,rng.uniform(-0.9,0.9),g*rng.uniform(0.2,1.0)*np.exp(-dt/span),0.25)
def glass_ring(t0,n,g,flo=1700,fhi=7500,dec=(0.06,0.4)):
    for _ in range(n):
        add(ping(rng.uniform(flo,fhi),rng.uniform(*dec)),t0+rng.uniform(0,0.02),rng.uniform(-0.8,0.8),g*rng.uniform(0.3,1.0),0.5)
for t0,g in ((T['i1'],0.62),(T['i2'],0.78),(T['i3'],0.95)):
    add(kick(0.9,150,38,0.035,0.28,1.8),t0,0,g*0.95,0.10)
    add(noise_band(0.25,150,900)*np.exp(-tt(0.25)/0.05),t0,0,g*0.55,0.2)              # the thud's body
    add(noise_band(0.08,2500,14000)*np.exp(-tt(0.08)/0.012),t0,0,g*0.7,0.3)          # the snap
    glass_ring(t0,9,0.10*g)
    cracks(t0+0.01,0.55,int(70*g),0.30*g)
# the glass under strain: ticks thicken, a whine climbs, the air rises
for _ in range(240):
    u=rng.uniform(0,1)**0.45; t0=T['i3']+0.15+u*(T['burst']-T['i3']-0.2)
    d=rng.uniform(0.002,0.008); c=noise_band(d+0.004,2500,12000)*np.exp(-tt(d+0.004)/(d/3))
    add(c,t0,rng.uniform(-1,1),0.14+0.36*u,0.2)
d=T['burst']-T['i3']-0.1; ts=tt(d); u=ts/d
for f0,f1,g in ((900,1900,0.12),(1340,2850,0.09),(2010,4300,0.06)):
    ph=2*np.pi*np.cumsum(f0*(f1/f0)**(u**1.5))/SR
    add(fade(np.sin(ph)*(u**2.2)*(1+0.3*np.sin(2*np.pi*(9+14*u)*ts)),0.01,0.03),T['i3']+0.1,0,g,0.5)
bands=[(200*1.6**k,200*1.6**(k+1)) for k in range(9)]
ris=np.zeros(len(ts))
for k,(lo,hi) in enumerate(bands):
    w=np.exp(-((k-(0.5+7.5*u**1.3))/1.1)**2)
    ris+=noise_band(d,lo,hi)*w
add(fade(ris*(u**2.2),0.01,0.035),T['i3']+0.1,0,1.25,0.3)

# ---- the burst
tb=T['burst']
add(kick(2.6,110,27,0.09,0.75,2.6),tb,0,1.0,0.08)                                     # the sub
add(kick(0.6,220,55,0.03,0.12,1.5),tb,0,0.6,0.1)                                      # the punch
ex=np.zeros(int(1.6*SR)); te=tt(1.6)
for k,(lo,hi) in enumerate(bands+[(200*1.6**9,16000)]):
    ex+=noise_band(1.6,lo,hi)*np.exp(-te/(0.9*0.72**k+0.05))*(1.0 if k<6 else 0.7)
add(ex,tb,0,0.55,0.35)
for _ in range(520):                                                                   # the glass
    dt=rng.exponential(0.42)
    if dt>2.6: continue
    f=np.exp(rng.uniform(np.log(1800),np.log(11500)))
    add(ping(f,rng.uniform(0.012,0.14)),tb+0.01+dt,rng.uniform(-1,1),0.085*rng.uniform(0.2,1.0)*np.exp(-dt/1.1),0.6)
add(noise_band(0.9,3500,15000)*np.exp(-tt(0.9)/0.16),tb,0,0.42,0.4)
for pan0,pan1,t0,g in ((-0.9,0.9,tb+0.22,0.34),(0.8,-0.8,tb+0.42,0.26),(-0.3,0.6,tb+0.75,0.16)):   # shards past the camera
    d=0.55; ts2=tt(d); u2=ts2/d; w=np.zeros(len(ts2))
    for k,(lo,hi) in enumerate(bands[:7]):
        w+=noise_band(d,lo,hi)*np.exp(-((k-(1.0+4.5*np.sin(np.pi*u2)))/1.0)**2)
    w*=np.sin(np.pi*u2)**2.2
    pan=pan0+(pan1-pan0)*u2; i=int(t0*SR); n=len(w)
    L[i:i+n]+=w*g*np.cos((pan+1)*np.pi/4); R[i:i+n]+=w*g*np.sin((pan+1)*np.pi/4)
    WL[i:i+n]+=w*g*0.2; WR[i:i+n]+=w*g*0.2

# ---- the logo: a wide D major chord out of the blast
def pad(freq,d,g,att,rel,bright):
    ts=tt(d); out_l=np.zeros(len(ts)); out_r=np.zeros(len(ts))
    for det,pn in ((-0.004,-0.7),(0.0,0.0),(0.004,0.7)):
        f=freq*(1+det); s=np.zeros(len(ts))
        for h in range(1,int(min(18,9000/f))+1):
            s+=np.sin(2*np.pi*f*h*ts+rng.uniform(0,6.28))/h**bright
        s*=1+0.08*np.sin(2*np.pi*rng.uniform(0.2,0.5)*ts+rng.uniform(0,6.28))
        out_l+=s*np.cos((pn+1)*np.pi/4); out_r+=s*np.sin((pn+1)*np.pi/4)
    e=np.minimum(ts/att,1)*np.exp(-np.maximum(ts-att,0)/rel)
    return out_l*e*g,out_r*e*g
t0=tb+0.15; d=DUR-t0; e=tt(d)
chord=np.zeros(len(e))
for f,g in ((110,.12),(165,.09),(220,.07),(330,.04)):
    chord+=g*np.sin(2*np.pi*f*e)*np.minimum(e/0.4,1)*np.exp(-e/1.2)
add(chord,t0,0,CHORD_GAIN,0.0)
ts=tt(d); air=noise_band(d,5000,14000)*np.minimum(ts/1.2,1)*np.exp(-ts/2.2)
add(air,t0,0,0.035,0.6)

# ---- the word: a sweep, then two bell notes
d=0.6; ts=tt(d); u=ts/d; sw=np.zeros(len(ts))
for k,(lo,hi) in enumerate(bands[3:]+[(200*1.6**9,15000)]):
    sw+=noise_band(d,lo,hi)*np.exp(-((k-(0.5+5.5*u))/1.0)**2)
i=int(T['text']*SR); pan=-0.6+1.2*u; w=sw*np.sin(np.pi*u)**1.5*0.16
L[i:i+len(w)]+=w*np.cos((pan+1)*np.pi/4); R[i:i+len(w)]+=w*np.sin((pan+1)*np.pi/4); WL[i:i+len(w)]+=w*0.4; WR[i:i+len(w)]+=w*0.4
def bell(f,dec,g,t0,pan):
    for mul,a,dm in ((1,1,1),(2.0,0.35,0.6),(2.76,0.28,0.45),(5.4,0.12,0.25),(8.93,0.05,0.15)):
        add(ping(f*mul,dec*dm),t0,pan,g*a,0.8)
bell(1318.5,0.9,0.085,T['text']+0.50,0.25); bell(1760.0,0.8,0.06,T['text']+0.62,-0.2); bell(880.0,1.3,0.06,T['text']+0.50,0.0)
for k,f in enumerate((2637.0,3520.0,4434.9,5274.0)):                                   # the glint
    bell(f,0.35,0.022,T['text']+1.05+0.085*k,-0.5+0.33*k)

# ---- reverb, master
irn=int(2.6*SR); ti=np.arange(irn)/SR
def ir(seed):
    r=np.random.default_rng(seed); x=r.standard_normal(irn); X=np.fft.rfft(x); f=np.fft.rfftfreq(irn,1/SR)
    X*=1/(1+(250/np.maximum(f,1))**2)*1/(1+(f/6500)**2); y=np.fft.irfft(X,irn)*(np.exp(-ti/0.55)*0.8+np.exp(-ti/0.12)*0.6)
    y[:int(0.012*SR)]*=np.linspace(0,1,int(0.012*SR)); return y/np.sqrt((y**2).sum())
def conv(x,h):
    n=len(x)+len(h); m=1<<int(np.ceil(np.log2(n))); return np.fft.irfft(np.fft.rfft(x,m)*np.fft.rfft(h,m),m)[:len(x)]
RL=conv(WL,ir(1)); RR=conv(WR,ir(2))
ML=L+RL*0.9; MR=R+RR*0.9
m=np.clip((T['end']-0.02-t)/(T['end']-0.02-T['fade']),0,1)**1.5*np.minimum(t/0.05,1)
ML*=m; MR*=m
pk=max(np.abs(ML).max(),np.abs(MR).max()); print('pre-limit peak',pk)
drive=1.35/pk*1.0
ML=np.tanh(ML*drive*1.6)/np.tanh(1.6); MR=np.tanh(MR*drive*1.6)/np.tanh(1.6)
pk=max(np.abs(ML).max(),np.abs(MR).max()); ML*=0.94/pk; MR*=0.94/pk
out=np.stack([ML,MR],1)
OUT=os.environ.get("OUT","audio.wav")
with wave.open(OUT,"wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes((out*32767).astype(np.int16).tobytes())
# a loudness curve, to check the shape
blk=int(SR*0.1); rms=[float(np.sqrt((out[i:i+blk]**2).mean())) for i in range(0,N,blk)]
print(' '.join('%2d'%int(max(-60,20*np.log10(r+1e-6))+60) for r in rms))
