// generated by Fast Light User Interface Designer (fluid) version 1.0303

#include "grab_console.h"
#include "commands.h"

Fl_Double_Window* GrabUserInterface::make_window() {
  Fl_Double_Window* w;
  { Fl_Double_Window* o = new Fl_Double_Window(642, 587);
    w = o;
    o->user_data((void*)(this));
    { Fl_Button *btn = new Fl_Button(325, 35, 100, 25, "Raise Cutter");
	btn->callback(press, (void*)&M_GrabMotor_Start);
    } // Fl_Button* o
    { Fl_Button *btn = new Fl_Button(325, 70, 100, 25, "Lower Cutter");
	btn->callback(press, (void*)&M_GrabMotor_Start);
    } // Fl_Button* o
    { Fl_Button *btn = new Fl_Button(215, 35, 90, 25, "Raise Grab");
	btn->callback(press, (void*)&M_GrabMotor_Start);
    } // Fl_Button* o
    { Fl_Button *btn = new Fl_Button(215, 70, 90, 25, "Lower Grab");
	btn->callback(press, (void*)&M_GrabMotor_Start);
    } // Fl_Button* o
    { new Fl_Button(40, 195, 120, 25, "Tip Bale");
    } // Fl_Button* o
    { Fl_Button *btn = new Fl_Button(195, 160, 180, 25, "Cutter Conveyor Forward");
	btn->callback(press, (void*)&MM_CutterConveyorForward_Run);
    } // Fl_Button* o
    { Fl_Button *btn = new Fl_Button(195, 195, 180, 25, "Cutter Conveyor Reverse");
	btn->callback(press, (void*)&MM_CutterConveyorReverse_Run);
    } // Fl_Button* o
    { new Fl_Button(195, 230, 180, 25, "Cutter Conveyor Stop");
    } // Fl_Button* o
    { Fl_Check_Button* o = new Fl_Check_Button(195, 275, 190, 25, "Cutter Conveyor Forward");
      o->down_box(FL_DOWN_BOX);
		o->callback(press, (void*)&MM_CutterConveyorForward_Run);
    } // Fl_Check_Button* o
    { Fl_Check_Button* o = new Fl_Check_Button(195, 310, 190, 25, "Cutter Conveyor Reverse");
      o->down_box(FL_DOWN_BOX);
		o->callback(press, (void*)&MM_CutterConveyorReverse_Run);
    } // Fl_Check_Button* o
    { Fl_Check_Button* o = new Fl_Check_Button(40, 165, 140, 25, "Automatic Mode");
      o->down_box(FL_DOWN_BOX);
    } // Fl_Check_Button* o
    o->end();
  } // Fl_Double_Window* o
  return w;
}


