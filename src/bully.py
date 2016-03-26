#! /usr/bin/env python

import sys
import os

try:
    from Tkinter import *
except ImportError:
    from tkinter import *

try:
    import ttk
    py3 = 0
except ImportError:
    import tkinter.ttk as ttk
    py3 = 1

import bully_support

def vp_start_gui():
    '''Starting point when module is the main routine.'''
    global val, w, root
    root = Tk()
    bully_support.set_Tk_var()
    top = Bully_Toplevel (root)
    bully_support.init(root, top)
    root.mainloop()

w = None
def create_Bully_Toplevel(root, *args, **kwargs):
    '''Starting point when module is imported by another program.'''
    global w, w_win, rt
    rt = root
    w = Toplevel (root)
    bully_support.set_Tk_var()
    top = Bully_Toplevel (w)
    bully_support.init(w, top, *args, **kwargs)
    return (w, top)

def destroy_Bully_Toplevel():
    global w
    w.destroy()
    w = None
    
bully_support.lockwait = 43


class Bully_Toplevel:
    def __init__(self, top=None):
        '''This class configures and populates the toplevel window.
           top is the toplevel containing window.'''
        _bgcolor = '#d9d9d9'  # X11 color: 'gray85'
        _fgcolor = '#000000'  # X11 color: 'black'
        _compcolor = '#d9d9d9' # X11 color: 'gray85'
        _ana1color = '#d9d9d9' # X11 color: 'gray85' 
        _ana2color = '#d9d9d9' # X11 color: 'gray85' 
        self.style = ttk.Style()

        self.style.map('.',background=
            [('selected', _compcolor), ('active',_ana2color)])

        top.geometry("480x146+752+360")
        top.title("Bully v1.1")

        self.BSSID = Entry(top)
        self.BSSID.place(relx=0.13, rely=0.07, relheight=0.14, relwidth=0.35)
        self.BSSID.configure(textvariable=bully_support.bssid)
        self.BSSID.configure(width=166)

        self.ESSID = Entry(top)
        self.ESSID.place(relx=0.63, rely=0.07, relheight=0.14, relwidth=0.35)
        self.ESSID.configure(textvariable=bully_support.essid)
        self.ESSID.configure(width=166)

        self.mes46 = Message(top)
        self.mes46.place(relx=0.0, rely=0.07, relheight=0.14, relwidth=0.13)
        self.mes46.configure(text='''BSSID:''')
        self.mes46.configure(width=63)

        self.Message2 = Message(top)
        self.Message2.place(relx=0.5, rely=0.07, relheight=0.14, relwidth=0.11)
        self.Message2.configure(text='''ESSID:''')
        self.Message2.configure(width=70)

        self.channel = ttk.Combobox(top)
        self.channel.place(relx=0.17, rely=0.21, relheight=0.12, relwidth=0.1)
        self.value_list = [1,2,3,4,5,6,7,8,9,10,11,12,13,14,]
        self.channel.configure(values=self.value_list)
        self.channel.configure(textvariable=bully_support.channel)
        self.channel.configure(width=47)
        self.channel.configure(takefocus="")
        self.channel.delete(0,"end")
        self.channel.insert(0,1)

        self.Message4 = Message(top)
        self.Message4.place(relx=0.54, rely=0.21, relheight=0.14, relwidth=0.08)
        self.Message4.configure(text='''PIN:''')
        self.Message4.configure(width=70)

        self.Entry1 = Entry(top)
        self.Entry1.place(relx=0.63, rely=0.21, relheight=0.14, relwidth=0.35)
        self.Entry1.configure(textvariable=bully_support.wps_pin)
        self.Entry1.configure(width=166)

        self.Message6 = Message(top)
        self.Message6.place(relx=0.29, rely=0.21, relheight=0.14, relwidth=0.12)
        self.Message6.configure(text='''Device:''')
        self.Message6.configure(width=70)

        self.Device = ttk.Combobox(top)
        self.Device.place(relx=0.4, rely=0.21, relheight=0.14, relwidth=0.14)
        self.Device_list = os.listdir('/sys/class/net/')
        self.Device.configure(values=self.Device_list)
        self.Device.configure(textvariable=bully_support.dev)
        self.Device.configure(width=66)

        self.Channel = Checkbutton(top)
        self.Channel.place(relx=0.0, rely=0.21, relheight=0.14, relwidth=0.16)
        self.Channel.configure(justify=LEFT)
        self.Channel.configure(text='''Channel:''')
        self.Channel.configure(variable=bully_support.use_chan)

        self.Force = Checkbutton(top)
        self.Force.place(relx=0.0, rely=0.34, relheight=0.14, relwidth=0.12)
        self.Force.configure(justify=LEFT)
        self.Force.configure(text='''Force''')
        self.Force.configure(variable=bully_support.force)

        self.sequential = Checkbutton(top)
        self.sequential.place(relx=0.0, rely=0.48, relheight=0.14, relwidth=0.19)
        self.sequential.configure(justify=LEFT)
        self.sequential.configure(text='''Sequential''')
        self.sequential.configure(variable=bully_support.sequential)

        self.bfcs = Checkbutton(top)
        self.bfcs.place(relx=0.19, rely=0.34, relheight=0.27, relwidth=0.27)
        self.bfcs.configure(justify=LEFT)
        self.bfcs.configure(text='''Brutefoce
Checksum digit''')
        self.bfcs.configure(variable=bully_support.bfcs)
        self.bfcs.configure(width=131)

        self.Message3 = Message(top)
        self.Message3.place(relx=0.48, rely=0.34, relheight=0.14, relwidth=0.15)
        self.Message3.configure(text='''Verbosity:''')
        self.Message3.configure(width=70)

        self.Message7 = Message(top)
        self.Message7.place(relx=0.48, rely=0.48, relheight=0.14, relwidth=0.14)
        self.Message7.configure(text='''Lockwait:''')
        self.Message7.configure(width=70)

        self.Verbosity = ttk.Combobox(top)
        self.Verbosity.place(relx=0.63, rely=0.34, relheight=0.12, relwidth=0.08)

        self.value_list = [1,2,3,4,]
        self.Verbosity.configure(values=self.value_list)
        self.Verbosity.configure(textvariable=bully_support.verbosity)
        self.Verbosity.configure(width=37)
        self.Verbosity.configure(takefocus="")
        self.Verbosity.delete(0,"end")
        self.Verbosity.insert(0,3)

        self.spi69 = Spinbox(top, from_=0, to=1000)
        self.spi69.place(relx=0.63, rely=0.48, relheight=0.14, relwidth=0.1)
        self.spi69.configure(textvariable=bully_support.lockwait)
        self.spi69.configure(to="1000")
        self.spi69.configure(width=48)
        self.spi69.delete(0,"end")
        self.spi69.insert(0,43)

        self.pixiewps = Checkbutton(top)
        self.pixiewps.place(relx=0.73, rely=0.34, relheight=0.14, relwidth=0.16)
        self.pixiewps.configure(justify=LEFT)
        self.pixiewps.configure(text='''Pixiewps''')
        self.pixiewps.configure(variable=bully_support.pixiewps)

        self.ignore_lockout = Checkbutton(top)
        self.ignore_lockout.place(relx=0.73, rely=0.48, relheight=0.14
                , relwidth=0.24)
        self.ignore_lockout.configure(justify=LEFT)
        self.ignore_lockout.configure(text='''Ignore Lockout''')
        self.ignore_lockout.configure(variable=bully_support.lock_ignore)

        self.Run = Button(top)
        self.Run.place(relx=0.02, rely=0.68, height=26, width=70)
        self.Run.configure(command=bully_support.run_cmd)
        self.Run.configure(text='''Run''')
        self.Run.configure(width=70)

        #self.About = Button(top)
        #self.About.place(relx=0.81, rely=0.68, height=26, width=72)
        #self.About.configure(command=bully_support.scan_ap)
        #self.About.configure(text='''Scan''')
        #self.About.configure(width=72)

        self.menubar = Menu(top,bg=_bgcolor,fg=_fgcolor)
        top.configure(menu = self.menubar)

        self.advanced = Menu(top,tearoff=0)
        self.menubar.add_cascade(menu=self.advanced,
                label="Advanced")
        self.advanced.add_checkbutton(
                variable=bully_support.no_ack,
                label="No Acks")
        self.advanced.add_checkbutton(
                variable=bully_support.detect_lock,
                label="Detect unreported locks")
        self.advanced.add_checkbutton(
                variable=bully_support.eap_fail,
                label="EAP Fail")
        self.advanced.add_checkbutton(
                variable=bully_support.no_fcs,
                label="No FCS")
        self.advanced.add_checkbutton(
                variable=bully_support.w7_reg,
                label="Emulate W7 Registrar")
        self.advanced.add_checkbutton(
                variable=bully_support.radiotap,
                label="Assume Radiotap Headers")
        self.advanced.add_checkbutton(
                variable=bully_support.supress,
                #command=bully_support.TODO,
                label="Supress packet throttling")
        #self.menubar.add_command(
        #        command=bully_support.about_us,
        #        label="About")


        self.Message1 = Message(top)
        self.Message1.place(relx=0.21, rely=0.68, relheight=0.14, relwidth=0.08)
        self.Message1.configure(text='''Wait''')
        self.Message1.configure(width=70)

        self.Spinbox1 = Spinbox(top, from_=0, to=1000)
        self.Spinbox1.place(relx=0.29, rely=0.68, relheight=0.14, relwidth=0.08)
        self.Spinbox1.configure(textvariable=bully_support.m5_1s)
        self.Spinbox1.configure(to="1000")
        self.Spinbox1.configure(width=38)
        self.Spinbox1.delete(0,"end")
        self.Spinbox1.insert(0,0)

        self.Message5 = Message(top)
        self.Message5.place(relx=0.38, rely=0.68, relheight=0.14, relwidth=0.17)
        self.Message5.configure(text='''seconds per''')
        self.Message5.configure(width=83)

        self.Spinbox2 = Spinbox(top, from_=1, to=1000)
        self.Spinbox2.place(relx=0.56, rely=0.68, relheight=0.14, relwidth=0.08)
        self.Spinbox2.configure(textvariable=bully_support.m5_1a)
        self.Spinbox2.configure(to="1000")
        self.Spinbox2.configure(width=38)
        self.Spinbox2.delete(0,"end")
        self.Spinbox2.insert(0,1)

        self.Message8 = Message(top)
        self.Message8.place(relx=0.65, rely=0.68, relheight=0.14, relwidth=0.13)
        self.Message8.configure(text='''attempt''')
        self.Message8.configure(width=70)






if __name__ == '__main__':
    vp_start_gui()



