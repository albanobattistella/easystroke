#include "main.h"
#include "win.h"
#include "strokeaction.h"
#include "actiondb.h"
#include "prefdb.h"
#include "trace.h"
#include "copy.h"
#include "shape.h"
#include "grabber.h"

#include <X11/Xutil.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xrandr.h>
#include <X11/Xproto.h>

#include <signal.h>
#include <fcntl.h>
#include <getopt.h>

bool gui = true;
extern bool no_xi;
bool experimental = false;
int verbosity = 0;

Display *dpy;

int fdw;

std::string config_dir;
Win *win;

void send(char c) {
	char cs[2] = {c, 0};
	write(fdw, cs, 1);
}

int (*oldHandler)(Display *, XErrorEvent *) = 0;

int xErrorHandler(Display *dpy2, XErrorEvent *e) {
	if (dpy != dpy2) {
		return oldHandler(dpy2, e);
	}
	if (verbosity == 0 && e->error_code == BadWindow && e->request_code == X_ChangeWindowAttributes)
		return 0;
	char text[64];
	XGetErrorText(dpy, e->error_code, text, sizeof text);
	char msg[16];
	snprintf(msg, sizeof msg, "%d", e->request_code);
	char def[32];
	snprintf(def, sizeof def, "request_code=%d", e->request_code);
	char dbtext[128];
	XGetErrorDatabaseText(dpy, "XRequest", msg,
			def, dbtext, sizeof dbtext);
	std::cerr << "XError: " << text << ": " << dbtext << std::endl;
	return 0;

}


void xi_warn() {
	if (no_xi)
		return;
	static bool warned = false;
	if (warned)
		return;
	printf("warning: Xinput extension not working correctly\n");
	warned = true;
}

Glib::Thread *main_thread = 0;

void run_gui() {
	win = new Win;
	Gtk::Main::run();
	gui = false;
	delete win;
	send(P_QUIT);
	main_thread->join();
}

void quit(int) {
	if (gui)
		win->quit();
	else
		send(P_QUIT);
}

Trace *init_trace() {
	switch(prefs().trace.get()) {
		case TraceNone:
			return new Trivial();
		case TraceShape:
			return new Shape();
		default:
			return new Copy();
	}
}

Window current = 0;
bool ignore = false;
bool scroll = false;
guint press_button = 0;
Trace *trace = 0;
bool in_proximity = false;

void handle_stroke(RStroke stroke, int trigger, int button);
void set_timeout(long us);

class Handler {
protected:
	Handler *child;
protected:
	virtual void grab() {}
	virtual void resume() { grab(); }
	virtual std::string name() = 0;
public:
	Handler *parent;
	Handler() : child(0), parent(0) {}
	Handler *top() {
		if (child)
			return child->top();
		else
			return this;
	}
	virtual void motion(int x, int y, Time t) {}
	virtual void press(guint b, int x, int y, Time t) {}
	virtual void release(guint b, int x, int y, Time t) {}
	virtual void press_repeated() {}
	virtual void pressure() {}
	virtual void proximity_out() {}
	virtual void timeout() {}
	void replace_child(Handler *c) {
		bool had_child = child;
		if (child)
			delete child;
		child = c;
		if (child)
			child->parent = this;
		if (verbosity >= 2) {
			std::string stack;
			for (Handler *h = child ? child : this; h; h=h->parent) {
				stack = h->name() + " " + stack;
			}
			std::cout << "New event handling stack: " << stack << std::endl;
		}
		if (child)
			child->init();
		if (!child && had_child)
			resume();
	}
	virtual void init() { grab(); }
	virtual bool idle() { return false; }
	virtual bool only_xi() { return false; }
	virtual ~Handler() {
		if (child)
			delete child;
	}
};

class IgnoreHandler : public Handler {
public:
	void grab() {
		grabber->grab(Grabber::ALL_SYNC);
	}
	virtual void press(guint b, int x, int y, Time t) {
		XAllowEvents(dpy, ReplayPointer, CurrentTime);
		if (!in_proximity)
			proximity_out();
	}
	virtual void proximity_out() {
		clear_mods();
		grabber->grab(Grabber::ALL_SYNC);
		parent->replace_child(0);
	}
	virtual ~IgnoreHandler() {
		clear_mods();
	}
	virtual std::string name() { return "Ignore"; }
};

class WaitForButtonHandler : public Handler {
	guint button;
	bool down;
public:
	WaitForButtonHandler(guint b, bool d) : button(b), down(d) {}
	virtual void press(guint b, int x, int y, Time t) {
		XAllowEvents(dpy, AsyncPointer, t);
		if (!down)
			return;
		if (b == button)
			parent->replace_child(0);
	}
	virtual void release(guint b, int x, int y, Time t) {
		if (down)
			return;
		if (b == button)
			parent->replace_child(0);
	}
	virtual std::string name() { return "WaitForButton"; }
};

inline float abs(float x) { return x > 0 ? x : -x; }

class AbstractScrollHandler : public Handler {
	int last_x, last_y;
	Time last_t;
	float offset_x, offset_y;

protected:
	AbstractScrollHandler() : last_t(0), offset_x(0.0), offset_y(0.0) {}
	virtual void fake_button(int b1, int n1, int b2, int n2) {
		grabber->suspend();
		for (int i = 0; i<n1; i++) {
			XTestFakeButtonEvent(dpy, b1, True, CurrentTime);
			XTestFakeButtonEvent(dpy, b1, False, CurrentTime);
		}
		for (int i = 0; i<n2; i++) {
			XTestFakeButtonEvent(dpy, b2, True, CurrentTime);
			XTestFakeButtonEvent(dpy, b2, False, CurrentTime);
		}
		grabber->resume();
	}
	static float curve(float v) {
		return v * exp(log(abs(v))/3);
	}
public:
	virtual void motion(int x, int y, Time t) {
		if (!last_t || abs(x-last_x) > 100 || abs(y-last_y) > 100) {
			last_x = x;
			last_y = y;
			last_t = t;
			return;
		}
		if (t == last_t)
			return;
		offset_x += curve(float(x-last_x)/float(t-last_t))*float(t-last_t)/10.0;
		offset_y += curve(float(y-last_y)/float(t-last_t))*float(t-last_t)/5.0;
		last_x = x;
		last_y = y;
		last_t = t;
		int b1 = 0, n1 = 0, b2 = 0, n2 = 0;
		if (abs(offset_x) > 1.0) {
			n1 = floor(abs(offset_x));
			if (offset_x > 0) {
				b1 = 7;
				offset_x -= n1;
			} else {
				b1 = 6;
				offset_x += n1;
			}
		}
		if (abs(offset_y) > 1.0) {
			if (abs(offset_y) < 1.0)
				return;
			n2 = floor(abs(offset_y));
			if (offset_y > 0) {
				b2 = 5;
				offset_y -= n2;
			} else {
				b2 = 4;
				offset_y += n2;
			}
		}
		if (n1 || n2)
			fake_button(b1,n1, b2,n2);
	}
};

class ScrollHandler : public AbstractScrollHandler {
	guint pressed, pressed2;
public:
	ScrollHandler() : pressed(0), pressed2(0) {
	}
	ScrollHandler(guint b, guint b2) : pressed(b), pressed2(b2) {
	}
	virtual void init() {
		if (pressed2) {
			XTestFakeButtonEvent(dpy, pressed, False, CurrentTime);
			XTestFakeButtonEvent(dpy, pressed2, False, CurrentTime);
		}
		grabber->grab(Grabber::POINTER);
		if (pressed2) {
			XTestFakeButtonEvent(dpy, pressed2, True, CurrentTime);
			XTestFakeButtonEvent(dpy, pressed, True, CurrentTime);
			replace_child(new WaitForButtonHandler(pressed2, true));
		}
	}
	virtual void fake_button(int b1, int n1, int b2, int n2) {
		if (pressed)
			XTestFakeButtonEvent(dpy, pressed, False, CurrentTime);
		if (pressed2)
			XTestFakeButtonEvent(dpy, pressed2, False, CurrentTime);
		AbstractScrollHandler::fake_button(b1,n1, b2,n2);
		if (pressed2)
			XTestFakeButtonEvent(dpy, pressed2, True, CurrentTime);
		if (pressed) {
			XTestFakeButtonEvent(dpy, pressed, True, CurrentTime);
			replace_child(new WaitForButtonHandler(pressed, true));
		}
	}
	virtual void press(guint b, int x, int y, Time t) {
		if (b != 4 && b != 5 && !pressed)
			pressed = b;
	}
	virtual void release(guint b, int x, int y, Time t) {
		if (b != pressed && b != pressed2)
			return;
		if (pressed2) {
			if (b == pressed) { // scroll button released, continue with Action
				XTestFakeButtonEvent(dpy, pressed, False, CurrentTime);
				XTestFakeButtonEvent(dpy, pressed2, False, CurrentTime);
				// Make sure event handling continues as usual
				Handler *p = parent;
				if (p->only_xi()) {
					p->replace_child(0);
					p->release(b,x,y,t);
				} else {
					grabber->grab(Grabber::BUTTON);
					XTestFakeButtonEvent(dpy, pressed2, True, CurrentTime);
					parent->replace_child(new WaitForButtonHandler(pressed2, true));
				}
			} else { // gesture button released, bail out
				clear_mods();
				XTestFakeButtonEvent(dpy, pressed, False, CurrentTime);
				XTestFakeButtonEvent(dpy, pressed2, False, CurrentTime);
				parent->parent->replace_child(0);
			}
		} else {
			clear_mods();
			parent->replace_child(0);
		}
	}
	virtual ~ScrollHandler() {
		clear_mods();
	}
	virtual std::string name() { return "Scroll"; }
};

std::set<guint> xinput_pressed;

class ActionHandler : public Handler {
	RStroke stroke;
	guint button, button2;

	void do_press() {
		handle_stroke(stroke, button2, button);
		ignore = false;
		if (scroll) {
			scroll = false;
			replace_child(new ScrollHandler(button, button2));
			return;
		}
		if (!press_button)
			return;
		XTestFakeButtonEvent(dpy, button, False, CurrentTime);
		XTestFakeButtonEvent(dpy, button2, False, CurrentTime);
		grabber->fake_button(press_button);
		press_button = 0;
		clear_mods();
		parent->replace_child(0);
	}
public:
	ActionHandler(RStroke s, guint b, guint b2) : stroke(s), button(b), button2(b2) {}

	virtual void init() {
		grabber->grab(Grabber::BUTTON);
		do_press();
	}

	virtual void press(guint b, int x, int y, Time t) {
		button = b;
		do_press();
	}

	virtual void resume() {
		grabber->grab(Grabber::BUTTON);
	}

	virtual void release(guint b, int x, int y, Time t) {
		if (b != button2) //TODO
			return;
		clear_mods();
		parent->replace_child(0);
	}
	virtual ~ActionHandler() {
		clear_mods();
	}

	virtual std::string name() { return "Action"; }
};

class ScrollXiHandler : public AbstractScrollHandler {
protected:
	void grab() {
		grabber->grab(Grabber::ALL_ASYNC);
	}
public:
	virtual void release(guint b, int x, int y, Time t) {
		Handler *p = parent;
		p->replace_child(0);
		p->release(b,x,y,t);
	}
	virtual std::string name() { return "ScrollXi"; }
};

class ScrollProxHandler : public AbstractScrollHandler {
protected:
	void grab() {
		grabber->grab(Grabber::ALL_ASYNC);
	}
public:
	virtual void init() {
		grabber->grab_xi_devs(true);
		grab();
	}
	virtual void motion(int x, int y, Time t) {
		if (xinput_pressed.size())
			AbstractScrollHandler::motion(x,y,t);
	}
	virtual void press(guint b, int x, int y, Time t) {
		XTestFakeButtonEvent(dpy, b, False, CurrentTime);
	}
	virtual void proximity_out() {
		parent->replace_child(0);
	}
	virtual std::string name() { return "ScrollProx"; }
	virtual ~ScrollProxHandler() {
		clear_mods();
		grabber->grab_xi_devs(false);
	}
};

class ActionXiHandler : public Handler {
	RStroke stroke;
	int emulated_button;

	guint button, button2;
public:
	ActionXiHandler(RStroke s, guint b, guint b2, Time t) : stroke(s), emulated_button(0), button(b), button2(b2) {
		XTestFakeButtonEvent(dpy, button, False, CurrentTime);
		XTestFakeButtonEvent(dpy, button2, False, CurrentTime);
	}
	virtual void init() {
		grabber->grab_xi_devs(true);
		handle_stroke(stroke, button2, button);
		ignore = false;
		if (scroll) {
			scroll = false;
			Handler *h = new ScrollXiHandler;
			replace_child(h);
			h->replace_child(new WaitForButtonHandler(button2, false));
			return;
		}
		if (!press_button) {
			grabber->grab(Grabber::ALL_ASYNC);
			return;
		}
		grabber->suspend();
		XTestFakeButtonEvent(dpy, press_button, True, CurrentTime);
		grabber->grab(Grabber::ALL_ASYNC);
		grabber->resume();
		emulated_button = press_button;
		press_button = 0;
	}
	virtual void press(guint b, int x, int y, Time t) {
		if (button2)
			return;
		button2 = b;
		XTestFakeButtonEvent(dpy, button2, False, CurrentTime);
		handle_stroke(stroke, button, button2);
		ignore = false;
		if (scroll) {
			scroll = false;
			Handler *h = new ScrollXiHandler;
			replace_child(h);
			// Why do we not need this?
//			h->replace_child(new WaitForButtonHandler(button2, false));
			return;
		}
		if (!press_button)
			return;
		if (emulated_button) {
			XTestFakeButtonEvent(dpy, emulated_button, False, CurrentTime);
			emulated_button = 0;
		}
		grabber->suspend();
		XTestFakeButtonEvent(dpy, press_button, True, CurrentTime);
		grabber->resume();
		emulated_button = press_button;
		press_button = 0;
	}
	virtual void resume() {
		grabber->grab(Grabber::ALL_ASYNC);
	}
	virtual void release(guint b, int x, int y, Time t) {
		if (b != button && b != button2)
			return;
		if (b == button)
			button = button2;
		button2 = 0;
		if (emulated_button) {
			XTestFakeButtonEvent(dpy, emulated_button, False, CurrentTime);
			emulated_button = 0;
		}
		if (!button && !button2)
			parent->replace_child(0);
	}
	virtual ~ActionXiHandler() {
		if (emulated_button) {
			XTestFakeButtonEvent(dpy, emulated_button, False, CurrentTime);
			emulated_button = 0;
		}
		clear_mods();
		grabber->grab_xi_devs(false);
	}
	virtual bool only_xi() { return true; }
	virtual std::string name() { return "ActionXi"; }
};

Trace::Point orig;

struct ButtonTime {
	guint b;
	Time t;
};

Bool is_xi_press(Display *dpy, XEvent *ev, XPointer arg) {
	ButtonTime *bt = (ButtonTime *)arg;
	if (!grabber->xinput)
		return false;
	if (!grabber->is_event(ev->type, Grabber::DOWN))
		return false;
	XDeviceButtonEvent* bev = (XDeviceButtonEvent *)ev;
	if (bev->button != bt->b)
		return false;
	return bev->time == bt->t;
}

Atom _NET_ACTIVE_WINDOW, WINDOW, ATOM, _NET_WM_WINDOW_TYPE, _NET_WM_WINDOW_TYPE_DOCK, _NET_WM_STATE, _NET_WM_STATE_FULLSCREEN;

Atom get_atom(Window w, Atom prop) {
	Atom actual_type;
	int actual_format;
	unsigned long nitems, bytes_after;
	unsigned char *prop_return = NULL;

	if (XGetWindowProperty(dpy, w, prop, 0, sizeof(Atom), False, ATOM, &actual_type, &actual_format,
				&nitems, &bytes_after, &prop_return) != Success)
		return None;
	if (!prop_return)
		return None;
	Atom atom = *(Atom *)prop_return;
	XFree(prop_return);
	return atom;
}

Window get_window(Window w, Atom prop) {
	Atom actual_type;
	int actual_format;
	unsigned long nitems, bytes_after;
	unsigned char *prop_return = NULL;

	if (XGetWindowProperty(dpy, w, prop, 0, sizeof(Atom), False, WINDOW, &actual_type, &actual_format,
				&nitems, &bytes_after, &prop_return) != Success)
		return None;
	if (!prop_return)
		return None;
	Window ret = *(Window *)prop_return;
	XFree(prop_return);
	return ret;
}

void activate_window(Window w, Time t) {
	Atom window_type = get_atom(w, _NET_WM_WINDOW_TYPE);
	if (window_type == _NET_WM_WINDOW_TYPE_DOCK)
		return;
	XWMHints *wm_hints = XGetWMHints(dpy, w);
	if (wm_hints) {
		bool input = wm_hints->input;
		XFree(wm_hints);
		if (!input)
			return;
	}
	Atom wm_state = get_atom(w, _NET_WM_STATE);
	if (wm_state != _NET_WM_STATE_FULLSCREEN)
		XSetInputFocus(dpy, current, RevertToParent, t);
}

class StrokeHandler : public Handler {
	guint button;
	RPreStroke cur;
	bool is_gesture;
	float speed;
	Time last_t;
	int last_x, last_y;
	bool repeated;
	bool have_xi;

	RStroke finish(guint b) {
		trace->end();
		XFlush(dpy);
		if (have_xi)
			XAllowEvents(dpy, AsyncPointer, CurrentTime);
		if (!is_gesture)
			cur->clear();
		if (b && prefs().advanced_ignore.get())
			cur->clear();
		return Stroke::create(*cur, button, b);
	}

	virtual void timeout() {
		if (verbosity >= 2)
			printf("Aborting stroke...\n");
		trace->end();
		XAllowEvents(dpy, ReplayPointer, CurrentTime);
		usleep(2000);
		XSync(dpy,False);
		XTestFakeRelativeMotionEvent(dpy, 0, 0, CurrentTime);
		parent->replace_child(0);
	}

	bool calc_speed(int x, int y, Time t) {
		if (!have_xi)
			return false;
		int dt = t - last_t;
		float c = exp(dt/-250.0);
		if (dt) {
			float dist = hypot(x-last_x, y-last_y);
			speed = c * speed + (1-c) * dist/dt;
		} else {
			speed = c * speed;
		}
		last_x = x;
		last_y = y;
		last_t = t;

		if (speed < 0.04) {
			timeout();
			return true;
		}
		long us = -250000.0*log(0.04/speed);
		set_timeout(us);
		return false;
	}
protected:
	virtual void press_repeated() {
		repeated = true;
	}
	virtual void pressure() {
		trace->end();
		XAllowEvents(dpy, ReplayPointer, CurrentTime);
		parent->replace_child(0);
	}
	virtual void motion(int x, int y, Time t) {
		if (!repeated && xinput_pressed.count(button) && !prefs().ignore_grab.get()) {
			if (verbosity >= 2)
				printf("Ignoring xi-only stroke\n");
			parent->replace_child(0);
			return;
		}
		cur->add(x,y,t);
		if (!is_gesture && hypot(x-orig.x, y-orig.y) > prefs().radius.get()) {
			is_gesture = true;
			bool first = true;
			for (std::vector<Stroke::Point>::iterator i = cur->points.begin(); i != cur->points.end(); i++) {
				Trace::Point p;
				p.x = i->x;
				p.y = i->y;
				if (first) {
					trace->start(p);
					first = false;
				} else {
					trace->draw(p);
				}
			}
		} else if (is_gesture) {
			Trace::Point p;
			p.x = x;
			p.y = y;
			trace->draw(p);
		}
		calc_speed(x,y,t);
	}

	virtual void press(guint b, int x, int y, Time t) {
		if (b == button)
			return;
		if (calc_speed(x,y,t))
			return;
		RStroke s = finish(b);

		if (gui && stroke_action()) {
			handle_stroke(s, button, b);
			parent->replace_child(0);
			return;
		}

		if (xinput_pressed.count(b)) {
			parent->replace_child(new ActionXiHandler(s, b, button, t));
		} else {
			xi_warn();
			parent->replace_child(new ActionHandler(s, b, button));
		}
	}

	virtual void release(guint b, int x, int y, Time t) {
		if (calc_speed(x,y,t))
			return;
		RStroke s = finish(b);

		handle_stroke(s, button, 0);
		if (ignore) {
			ignore = false;
			parent->replace_child(new IgnoreHandler);
			return;
		}
		if (scroll) {
			scroll = false;
			if (xinput_pressed.size() && in_proximity)
				parent->replace_child(new ScrollProxHandler);
			else
				parent->replace_child(new ScrollHandler);
			return;
		}
		if (press_button && !(!repeated && xinput_pressed.count(b) && press_button == button)) {
			grabber->fake_button(press_button);
			press_button = 0;
		}
		clear_mods();
		parent->replace_child(0);
	}
public:
	StrokeHandler(guint b, int x, int y, Time t) : button(b), is_gesture(false), speed(0.1), last_t(t), last_x(x), last_y(y),
	repeated(false), have_xi(false) {
		orig.x = x; orig.y = y;
		cur = PreStroke::create();
		cur->add(x,y,t);
		if (xinput_pressed.count(button))
			have_xi = true;
		XEvent ev;
		if (!have_xi) {
			ButtonTime bt;
			bt.b = b;
			bt.t = t;
			have_xi = XCheckIfEvent(dpy, &ev, &is_xi_press, (XPointer)&bt);
			if (have_xi)
				repeated = true;
		}
		if (have_xi) {
			xinput_pressed.insert(button);
		} else {
			XAllowEvents(dpy, AsyncPointer, CurrentTime);
			xi_warn();
		}
	}
	~StrokeHandler() {
		trace->end();
		if (have_xi)
			XAllowEvents(dpy, AsyncPointer, CurrentTime);
	}
	virtual std::string name() { return "Stroke"; }
};

class WorkaroundHandler : public Handler {
public:
	virtual void press(guint b, int x, int y, Time t) {
		if (b == 1)
			return;
		RPreStroke p = PreStroke::create();
		RStroke s = Stroke::create(*p, b, 1);
		parent->replace_child(new ActionXiHandler(s, 1, b, t));
	}
	virtual std::string name() { return "Workaround"; }
};

class IdleHandler : public Handler {
protected:
	virtual void init() {
		XGrabKey(dpy, XKeysymToKeycode(dpy,XK_Escape), AnyModifier, ROOT, True, GrabModeAsync, GrabModeSync);
		grab();
	}
	virtual void press(guint b, int x, int y, Time t) {
		if (!grabber->is_grabbed(b))
			if (b != 1)
				return;
			else { // b == 1
				if (grabber->get_device_button_state() & ~2) {
					XAllowEvents(dpy, AsyncPointer, t);
					replace_child(new WorkaroundHandler);
					return;
				} else {
					XAllowEvents(dpy, ReplayPointer, t);
					return;
				}
			}
		if (current)
			activate_window(current, t);
		replace_child(new StrokeHandler(b, x, y, t));
	}
	virtual void grab() {
		grabber->grab(Grabber::BUTTON);
	}
	virtual void resume() {
		grab();
	}
public:
	virtual bool idle() { return true; }
	virtual std::string name() { return "Idle"; }
	virtual ~IdleHandler() {
		XUngrabKey(dpy, XKeysymToKeycode(dpy,XK_Escape), AnyModifier, ROOT);
	}
};

class Main {
	std::string parse_args_and_init_gtk(int argc, char **argv);
	void create_config_dir();
	char* next_event();
	void usage(char *me, bool good);

	std::string display;
	Gtk::Main *kit;
	int fdr;
	int event_basep;
	bool randr;
public:
	Main(int argc, char **argv);
	void run();
	~Main();
};

Glib::Mutex *grabber_mutex = 0; //TODO: This is a hack

Main::Main(int argc, char **argv) : kit(0) {
	if (0) {
		RStroke trefoil = Stroke::trefoil();
		trefoil->draw_svg("easystroke.svg");
		exit(EXIT_SUCCESS);
	}
	display = parse_args_and_init_gtk(argc, argv);
	create_config_dir();
	grabber_mutex = new Glib::Mutex;
	grabber_mutex->lock();

 	int fds[2];
 	pipe(fds);
 	fdr = fds[0];
 	fcntl(fdr, F_SETFL, O_NONBLOCK);
 	fdw = fds[1];

 	signal(SIGINT, &quit);
}

void Main::usage(char *me, bool good) {
	printf("Usage: %s [OPTION]...\n", me);
	printf("  -c, --config-dir       Directory for config files\n");
	printf("      --display          X Server to contact\n");
	printf("  -x  --no-xi            Don't use the Xinput extension\n");
	printf("  -e  --experimental     Start in experimental mode\n");
	printf("  -n, --no-gui           Don't start the gui\n");
	printf("  -v, --verbose          Increase verbosity level\n");
	exit(good ? EXIT_SUCCESS : EXIT_FAILURE);
}

std::string Main::parse_args_and_init_gtk(int argc, char **argv) {
	static struct option long_opts1[] = {
		{"display",1,0,'d'},
		{"help",0,0,'h'},
		{"no-gui",1,0,'n'},
		{"no-xi",1,0,'x'},
		{0,0,0,0}
	};
	static struct option long_opts2[] = {
		{"config-dir",1,0,'c'},
		{"display",1,0,'d'},
		{"experimental",0,0,'e'},
		{"no-gui",0,0,'n'},
		{"no-xi",0,0,'x'},
		{"verbose",0,0,'v'},
		{0,0,0,0}
	};
	std::string display;
	char opt;
	// parse --display here, before Gtk::Main(...) takes it away from us
	opterr = 0;
	while ((opt = getopt_long(argc, argv, "nhx", long_opts1, 0)) != -1)
		switch (opt) {
			case 'd':
				display = optarg;
				break;
			case 'n':
				gui = false;
				break;
			case 'h':
				usage(argv[0], true);
				break;
			case 'x':
				no_xi = true;
				break;
		}
	optind = 1;
	opterr = 1;
	XInitThreads();
	Glib::thread_init();
	if (gui)
		kit = new Gtk::Main(argc, argv);
	oldHandler = XSetErrorHandler(xErrorHandler);

	while ((opt = getopt_long(argc, argv, "c:envx", long_opts2, 0)) != -1) {
		switch (opt) {
			case 'c':
				config_dir = optarg;
				break;
			case 'e':
				experimental = true;
				break;
			case 'v':
				verbosity++;
				break;
			case 'd':
			case 'n':
			case 'x':
				break;
			default:
				usage(argv[0], false);
		}
	}
	return display;
}

void Main::create_config_dir() {
	struct stat st;
	if (config_dir == "") {
		config_dir = getenv("HOME");
		config_dir += "/.easystroke";
	}
	if (lstat(config_dir.c_str(), &st) == -1) {
		if (mkdir(config_dir.c_str(), 0777) == -1) {
			printf("Error: Couldn't create configuration directory \"%s\"\n", config_dir.c_str());
			exit(EXIT_FAILURE);
		}
	} else {
		if (!S_ISDIR(st.st_mode)) {
			printf("Error: \"%s\" is not a directory\n", config_dir.c_str());
			exit(EXIT_FAILURE);
		}
	}
	config_dir += "/";
}

void handle_stroke(RStroke s, int trigger, int button) {
	s->trigger = trigger;
	s->button = (button == trigger) ? 0 : button;
	if (verbosity >= 4)
		s->print();
	if (gui) {
		if (!stroke_action()(s)) {
			Ranking ranking = actions().handle(s);
			if (ranking.id == -1)
				press_button = trigger;
			if (ranking.id != -1 || prefs().show_clicks.get())
				win->stroke_push(ranking);
		}
		win->icon_push(s);
	} else {
		if (actions().handle(s).id == -1)
			press_button = trigger;
	}
}

timeval timeout;
bool timeout_set;

void set_timeout(long us) {
	timeout.tv_sec = us / 1000000;
	timeout.tv_usec = us % 1000000;
	timeout_set = true;
}

char* Main::next_event() {
	static char buffer[2];
	int fdx = ConnectionNumber(dpy);
	fd_set fds;
	FD_ZERO(&fds);
	FD_SET(fdr, &fds);
	FD_SET(fdx, &fds);
	int n = 1 + ((fdr>fdx) ? fdr : fdx);
	while (!XPending(dpy)) {
		if (select(n, &fds, 0, 0, timeout_set ? &timeout : 0) == 0) {
			timeout_set = false;
			buffer[0] = P_TIMEOUT;
			return buffer;
		}
		if (read(fdr, buffer, 1) > 0)
			return buffer;
	}
	return 0;
}


void Main::run() {
	dpy = XOpenDisplay(display.c_str());
	if (!dpy) {
		printf("Couldn't open display\n");
		exit(EXIT_FAILURE);
	}

	actions().read();
	prefs().read();

	grabber = new Grabber;
	grabber_mutex->unlock();
	grabber->grab(Grabber::BUTTON);

	int error_basep;
	randr = XRRQueryExtension(dpy, &event_basep, &error_basep);
	if (randr)
		XRRSelectInput(dpy, ROOT, RRScreenChangeNotifyMask);

	trace = init_trace();

	_NET_ACTIVE_WINDOW = XInternAtom(dpy, "_NET_ACTIVE_WINDOW", False);
	ATOM = XInternAtom(dpy, "ATOM", False);
	WINDOW = XInternAtom(dpy, "WINDOW", False);
	_NET_WM_WINDOW_TYPE = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
	_NET_WM_WINDOW_TYPE_DOCK = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
	_NET_WM_STATE = XInternAtom(dpy, "_NET_WM_STATE", False);
	_NET_WM_STATE_FULLSCREEN = XInternAtom(dpy, "_NET_WM_STATE_FULLSCREEN", False);

	Handler *handler = new IdleHandler;
	handler->init();
	bool alive = true;

	Time last_time = 0;
	int last_type = 0;
	guint last_button = 0;
	if (verbosity >= 2)
		printf("Entering main loop...\n");
	while (alive || !handler->idle()) {
		char *ret = next_event();
		if (ret) {
			if (*ret == P_QUIT) {
				if (alive) {
					alive = false;
				} else {
					printf("Forcing easystroke to quit\n");
					//handler->cancel();
					delete handler;
					handler = new IdleHandler;
				}
				continue;
			}
			if (*ret == P_REGRAB)
				grabber->regrab();
			if (*ret == P_SUSPEND_GRAB)
				grabber->suspend();
			if (*ret == P_RESTORE_GRAB)
				grabber->resume();
			if (*ret == P_UPDATE_CURRENT) {
				prefs().write();
				grabber->update(current);
			}
			if (*ret == P_UPDATE_TRACE) {
				Trace *new_trace = init_trace();
				delete trace;
				trace = new_trace;
			}
			if (*ret == P_TIMEOUT) {
				handler->top()->timeout();
			}
			if (*ret == P_PROXIMITY) {
				grabber->select_proximity();
			}
			if (*ret == P_SCAN_WINDOWS) {
				grabber->scan_windows();
			}
			continue;
		}
		XEvent ev;
		XNextEvent(dpy, &ev);

		try {
		switch(ev.type) {
			case MotionNotify:
				if (verbosity >= 3)
					printf("Motion: (%d, %d)\n", ev.xmotion.x, ev.xmotion.y);
				if (handler->top()->only_xi())
					break;
				if (last_type == MotionNotify && last_time == ev.xmotion.time) {
					break;
				}
				handler->top()->motion(ev.xmotion.x, ev.xmotion.y, ev.xmotion.time);
				last_type = MotionNotify;
				last_time = ev.xmotion.time;
				break;

			case ButtonPress:
				if (verbosity >= 3)
					printf("Press: %d\n", ev.xbutton.button);
				if (handler->top()->only_xi())
					break;
				if (last_type == ButtonPress && last_time == ev.xbutton.time && last_button == ev.xbutton.button) {
					handler->top()->press_repeated();
					break;
				}
				handler->top()->press(ev.xbutton.button, ev.xbutton.x, ev.xbutton.y, ev.xbutton.time);
				last_type = ButtonPress;
				last_time = ev.xbutton.time;
				last_button = ev.xbutton.button;
				break;

			case ButtonRelease:
				if (verbosity >= 3)
					printf("Release: %d\n", ev.xbutton.button);
				if (handler->top()->only_xi())
					break;
				if (last_type == ButtonRelease && last_time == ev.xbutton.time && last_button == ev.xbutton.button)
					break;
				handler->top()->release(ev.xbutton.button, ev.xbutton.x, ev.xbutton.y, ev.xbutton.time);
				// TODO: Do we need this?
				xinput_pressed.erase(ev.xbutton.button);
				last_type = ButtonRelease;
				last_time = ev.xbutton.time;
				last_button = ev.xbutton.button;
				break;

			case KeyPress:
				if (ev.xkey.keycode != XKeysymToKeycode(dpy, XK_Escape))
					break;
				XAllowEvents(dpy, ReplayKeyboard, CurrentTime);
				if (handler->top()->idle())
					break;
				printf("Escape pressed: Resetting...\n");
				handler->replace_child(0);
				for (int i = 1; i <= 9; i++)
					XTestFakeButtonEvent(dpy, i, False, CurrentTime);
				XAllowEvents(dpy, AsyncPointer, CurrentTime);
				break;

			case ClientMessage:
				break;

			case EnterNotify:
				if (ev.xcrossing.mode == NotifyGrab)
					break;
				if (ev.xcrossing.detail == NotifyInferior)
					break;
				current = ev.xcrossing.window;
				grabber->update(current);
				break;

			case CreateNotify:
				grabber->create(ev.xcreatewindow.window);
				break;

			case DestroyNotify:
				break;
			default:
				if (randr && ev.type == event_basep) {
					XRRUpdateConfiguration(&ev);
					Trace *new_trace = init_trace();
					delete trace;
					trace = new_trace;
				}
				if (grabber->is_event(ev.type, Grabber::DOWN)) {
					XDeviceButtonEvent* bev = (XDeviceButtonEvent *)&ev;
					if (verbosity >= 3)
						printf("Press (Xi): %d\n", bev->button);
					xinput_pressed.insert(bev->button);
					if (last_type == ButtonPress && last_time == bev->time && last_button == bev->button) {
						handler->top()->press_repeated();
						break;
					}
					handler->top()->press(bev->button, bev->x, bev->y, bev->time);
					last_type = ButtonPress;
					last_time = bev->time;
					last_button = bev->button;
				}
				if (grabber->is_event(ev.type, Grabber::UP)) {
					XDeviceButtonEvent* bev = (XDeviceButtonEvent *)&ev;
					if (verbosity >= 3)
						printf("Release (Xi): %d\n", bev->button);
					if (last_type == ButtonRelease && last_time == bev->time && last_button == bev->button)
						break;
					handler->top()->release(bev->button, bev->x, bev->y, bev->time);
					xinput_pressed.erase(bev->button);
					last_type = ButtonRelease;
					last_time = bev->time;
					last_button = bev->button;
				}
				if (grabber->is_event(ev.type, Grabber::MOTION)) {
					XDeviceMotionEvent* mev = (XDeviceMotionEvent *)&ev;
					if (verbosity >= 3)
						printf("Motion (Xi): (%d, %d, %d)\n", mev->x, mev->y, mev->axis_data[2]);
					Grabber::XiDevice *xi_dev = grabber->get_xi_dev(mev->deviceid);
					if (xi_dev && xi_dev->supports_pressure && prefs().pressure_abort.get())
						if (xi_dev->normalize_pressure(mev->axis_data[2]) >=
								prefs().pressure_threshold.get())
						       handler->top()->pressure();
					if (last_type == MotionNotify && last_time == mev->time)
						break;
					handler->top()->motion(mev->x, mev->y, mev->time);
					last_type = MotionNotify;
					last_time = mev->time;
				}
				if (grabber->proximity_selected) {
					if (grabber->is_event(ev.type, Grabber::PROX_IN)) {
						in_proximity = true;
						if (verbosity >= 3)
							printf("Proximity: In\n");
					}
					if (grabber->is_event(ev.type, Grabber::PROX_OUT)) {
						in_proximity = false;
						if (verbosity >= 3)
							printf("Proximity: Out\n");
						handler->top()->proximity_out();
					}
				}
				break;
		}
		} catch (GrabFailedException) {
			printf("Error: A grab failed.  Resetting...\n");
			handler->replace_child(0);
		}
	}
	delete grabber;
	XCloseDisplay(dpy);
}

Main::~Main() {
	delete kit;
}

int main(int argc, char **argv) {
	Main mn(argc, argv);

	if (gui) {
		main_thread = Glib::Thread::create(sigc::mem_fun(mn, &Main::run), true);
		run_gui();
	} else
		mn.run();
	if (verbosity >= 2)
		printf("Exiting...\n");
}

bool SendKey::run() {

	if (xtest) {
		press();
		XTestFakeKeyEvent(dpy, code, true, 0);
		XTestFakeKeyEvent(dpy, code, false, 0);
		return true;
	}

	if (!current)
		return true;
	XKeyEvent ev;
	ev.type = KeyPress;	/* KeyPress or KeyRelease */
	ev.display = dpy;	/* Display the event was read from */
	ev.window = current;	/* ``event'' window it is reported relative to */
	ev.root = ROOT;		/* ROOT window that the event occurred on */
	ev.time = CurrentTime;	/* milliseconds */
	XTranslateCoordinates(dpy, ROOT, current, orig.x, orig.y, &ev.x, &ev.y, &ev.subwindow);
	ev.x_root = orig.x;	/* coordinates relative to root */
	ev.y_root = orig.y;	/* coordinates relative to root */
	ev.state = mods;	/* key or button mask */
	ev.keycode = code;	/* detail */
	ev.same_screen = true;	/* same screen flag */
	XSendEvent(dpy, current, True, KeyPressMask, (XEvent *)&ev);
	ev.type = KeyRelease;	/* KeyPress or KeyRelease */
	XSendEvent(dpy, current, True, KeyReleaseMask, (XEvent *)&ev);
	return true;
}

bool Button::run() {
	if (1) {
		press();
		press_button = button;
		return true;
	}
	if (!current)
		return true;
	// Doesn't work!
	XButtonEvent ev;
	ev.type = ButtonPress;  /* ButtonPress or ButtonRelease */
	ev.display = dpy;	/* Display the event was read from */
	ev.window = current;	/* ``event'' window it is reported relative to */
	ev.root = ROOT;		/* ROOT window that the event occurred on */
	ev.time = CurrentTime;	/* milliseconds */
	XTranslateCoordinates(dpy, ROOT, current, orig.x, orig.y, &ev.x, &ev.y, &ev.subwindow);
	ev.x_root = orig.x;	/* coordinates relative to root */
	ev.y_root = orig.y;	/* coordinates relative to root */
	ev.state = mods;	/* key or button mask */
	ev.button = button;     /* detail */
	ev.same_screen = true;	/* same screen flag */

	XSendEvent(dpy, current, True, ButtonPressMask, (XEvent *)&ev);
	ev.type = ButtonRelease;/* ButtonPress or ButtonRelease */
	XSendEvent(dpy, current, True, ButtonReleaseMask, (XEvent *)&ev);
	return true;
}

bool Scroll::run() {
	press();
	scroll = true;
	return true;
}

struct does_that_really_make_you_happy_stupid_compiler {
	guint mask;
	guint sym;
} modkeys[] = {
	{GDK_SHIFT_MASK, XK_Shift_L},
	{GDK_CONTROL_MASK, XK_Control_L},
	{GDK_MOD1_MASK, XK_Alt_L},
	{GDK_MOD2_MASK, 0},
	{GDK_MOD3_MASK, 0},
	{GDK_MOD4_MASK, 0},
	{GDK_MOD5_MASK, 0},
	{GDK_SUPER_MASK, XK_Super_L},
	{GDK_HYPER_MASK, XK_Hyper_L},
	{GDK_META_MASK, XK_Meta_L},
};
int n_modkeys = 10;

guint mod_state = 0;

void set_mod_state(int new_state) {
	for (int i = 0; i < n_modkeys; i++) {
		guint mask = modkeys[i].mask;
		if ((mod_state & mask) ^ (new_state & mask))
			XTestFakeKeyEvent(dpy, XKeysymToKeycode(dpy, modkeys[i].sym), new_state & mask, 0);
	}
	mod_state = new_state;
}

void ButtonInfo::press() {
	set_mod_state(state);
}

void ModAction::press() {
	set_mod_state(mods);
}

void clear_mods() {
	set_mod_state(0);
}

bool Ignore::run() {
	press();
	ignore = true;
	return true;
}
