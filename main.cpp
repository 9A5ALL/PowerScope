#include <gtkmm.h>
#include <cairomm/context.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>
#include <array>
#ifndef POWERSCOPE_VERSION
#define POWERSCOPE_VERSION "dev"
#endif
namespace fs=std::filesystem;
static std::string readstr(const fs::path& p){std::ifstream f(p);std::string s;std::getline(f,s);return s;}
static double number(const fs::path& p,double fallback=-1){try{return std::stod(readstr(p));}catch(...){return fallback;}}
static std::string fmt(double n,int d=1){if(!std::isfinite(n)||n<0)return "N/A";std::ostringstream s;s<<std::fixed<<std::setprecision(d)<<n;return s.str();}
static bool digits(const std::string& s){return !s.empty()&&std::all_of(s.begin(),s.end(),[](unsigned char c){return c>='0'&&c<='9';});}
struct Proc {unsigned long long ticks=0,start=0;long rss=0;std::string name; std::map<std::string,unsigned long long> gpu_ns;};
// DRM fdinfo exposes cumulative engine busy time, not energy. Deduplicate open FDs
// referring to the same DRM client ID to avoid counting one client several times.
static void gpu_read(int pid, Proc& p) {
 std::set<std::string> clients; std::error_code ec;
 fs::path dir="/proc/"+std::to_string(pid)+"/fdinfo";
 for(auto& ent:fs::directory_iterator(dir,ec)) {
  std::ifstream f(ent.path()); if(!f)continue;
  std::string line, driver, client; std::map<std::string,unsigned long long> engines;
  while(std::getline(f,line)) {
   auto pos=line.find(':');if(pos==std::string::npos)continue;
   std::string key=line.substr(0,pos), val=line.substr(pos+1);
   std::istringstream in(val);
   if(key=="drm-driver")in>>driver;
   else if(key=="drm-client-id")in>>client;
   else if(key.rfind("drm-engine-",0)==0) {
    unsigned long long n=0;std::string unit; if(in>>n>>unit && unit=="ns")engines[key.substr(11)]=n;
   }
  }
  if((driver!="i915"&&driver!="xe")||client.empty()||!clients.insert(client).second)continue;
  for(auto& [engine,n]:engines)p.gpu_ns[engine]+=n;
 }
}
static bool proc_read(int pid,Proc& p){std::ifstream f("/proc/"+std::to_string(pid)+"/stat");std::string s;if(!std::getline(f,s))return false;auto l=s.find('('),r=s.rfind(')');if(l==std::string::npos||r==std::string::npos)return false;p.name=s.substr(l+1,r-l-1);std::istringstream in(s.substr(r+2));std::vector<std::string> v;std::string t;while(in>>t)v.push_back(t);if(v.size()<22)return false;try{p.ticks=std::stoull(v[11])+std::stoull(v[12]);p.start=std::stoull(v[19]);p.rss=std::stol(v[21]);}catch(...){return false;}return true;}
static std::pair<unsigned long long,unsigned long long> cpu_ticks(){std::ifstream f("/proc/stat");std::string line;std::getline(f,line);std::istringstream in(line);std::string cpu;in>>cpu;unsigned long long x=0,total=0,idle=0;int i=0;while(in>>x){total+=x;if(i==3||i==4)idle+=x;++i;}return {total,idle};}
static double cpu_temperature() {
 std::error_code ec;
 for(const auto& h:fs::directory_iterator("/sys/class/hwmon",ec)) {
  if(readstr(h.path()/"name")!="coretemp")continue;
  for(const auto& e:fs::directory_iterator(h.path(),ec)) {
   auto filename=e.path().filename().string();
   if(filename.size()<11 || filename.substr(filename.size()-6)!="_label")continue;
   if(readstr(e.path())!="Package id 0")continue;
   auto input=h.path()/(filename.substr(0,filename.size()-6)+"_input");
   double value=number(input);if(value>=0)return value/1000.;
  }
  double value=number(h.path()/"temp1_input");if(value>=0)return value/1000.;
 }
 return -1;
}
struct Sample {double watts=-1,battery=-1,cpu=-1,gpu=-1,temp=-1;};
class Graph:public Gtk::DrawingArea {
public:
 std::deque<Sample> data;bool visible[5]={true,true,true,true,true};int window=300; // seconds
protected:
 bool on_draw(const Cairo::RefPtr<Cairo::Context>& c) override {
  auto a=get_allocation();double w=a.get_width(),h=a.get_height();c->set_source_rgb(.10,.12,.16);c->paint();
  double l=55,r=93,t=20,b=30,pw=std::max(1.,w-l-r),ph=std::max(1.,h-t-b);
  int count=std::min((int)data.size(),window);double maxw=10;
  for(int i=(int)data.size()-count;i<(int)data.size();++i)if(data[i].watts>=0)maxw=std::max(maxw,std::ceil(data[i].watts/5.)*5.);
  c->select_font_face("Sans",Cairo::FONT_SLANT_NORMAL,Cairo::FONT_WEIGHT_NORMAL);c->set_font_size(10);
  for(int i=0;i<=5;++i){double y=t+ph*i/5.;c->set_source_rgba(1,1,1,.13);c->set_line_width(1);c->move_to(l,y);c->line_to(w-r,y);c->stroke();c->set_source_rgb(.78,.81,.87);c->move_to(4,y+4);c->show_text(fmt(maxw*(5-i)/5.,0)+" W");c->move_to(w-r+5,y+4);c->show_text(fmt(100.*(5-i)/5.,0)+"%");c->move_to(w-r+48,y+4);c->show_text(fmt(110.*(5-i)/5.,0)+"C");}
  c->set_source_rgb(.78,.81,.87);c->move_to(l,h-9);c->show_text("-"+std::to_string(window/60)+" min");c->move_to(w-r-24,h-9);c->show_text("now");
  auto line=[&](int metric,double red,double green,double blue){c->set_source_rgb(red,green,blue);c->set_line_width(2);bool started=false;for(int i=0;i<count;++i){auto& s=data[data.size()-count+i];double v=metric==0?s.watts:(metric==1?s.battery:(metric==2?s.cpu:(metric==3?s.gpu:s.temp)));if(v<0){if(started)c->stroke();started=false;continue;}double x=l+pw*(1.-(count-1-i)/(double)std::max(1,window-1));double y=t+ph*(1.-std::clamp(v/(metric==0?maxw:(metric==4?110.:100.)),0.,1.));if(!started){c->move_to(x,y);started=true;}else c->line_to(x,y);}if(started)c->stroke();};
  if(visible[0])line(0,.30,.86,.62);if(visible[1])line(1,.40,.67,1.0);if(visible[2])line(2,1.,.67,.31);if(visible[3])line(3,.78,.52,1.);if(visible[4])line(4,1.,.36,.38);return true;
 }
};
class Window:public Gtk::Window {
 Gtk::Box root{Gtk::ORIENTATION_VERTICAL,7},toolbar{Gtk::ORIENTATION_HORIZONTAL,8};
 Gtk::Label title,note;
 Gtk::Button about_button{"About"};
 Gtk::Box heading{Gtk::ORIENTATION_HORIZONTAL,8};
 std::array<Gtk::CheckButton*,5> legend_checks{};
 fs::path settings_file;
 Gtk::Box dashboard{Gtk::ORIENTATION_HORIZONTAL,10};
 Gtk::Frame battery_frame{"Battery"}, system_frame{"System"}, energy_frame{"Energy"};
 Gtk::Grid battery_grid,system_grid,energy_grid;
 Gtk::Box total_box{Gtk::ORIENTATION_VERTICAL,3};
 Gtk::Label total_value,total_status;
 Gtk::Label battery_pct,battery_energy,battery_time,battery_direction;
 Gtk::Label cpu_value,gpu_value,temp_value,process_value;
 Gtk::Label rapl_value,energy_info;
 Gtk::Box toggles{Gtk::ORIENTATION_HORIZONTAL,9};Gtk::ComboBoxText zoom;Graph graph;Gtk::ScrolledWindow scroll;Gtk::TreeView tree;
 struct Cols:Gtk::TreeModel::ColumnRecord{Gtk::TreeModelColumn<int> pid;Gtk::TreeModelColumn<Glib::ustring> name;Gtk::TreeModelColumn<double> cpu,cpu_total,mem,estimated,gpu,energy_j;Cols(){add(pid);add(name);add(cpu);add(cpu_total);add(mem);add(estimated);add(gpu);add(energy_j);}} cols;
 Glib::RefPtr<Gtk::ListStore> model;fs::path battery,rapl;std::map<int,Proc> previous;std::map<int,double> accumulated_j;
 std::chrono::steady_clock::time_point last;std::pair<unsigned long long,unsigned long long> lastcpu{0,0};
 double last_rapl=-1,package_watts=-1;unsigned int logical_cpus=1;long hz=sysconf(_SC_CLK_TCK),page=sysconf(_SC_PAGESIZE);
public:
 Window(){settings_file=fs::path(Glib::get_user_config_dir())/"powerscope"/"settings.ini";logical_cpus=std::max(1u,std::thread::hardware_concurrency());set_title(std::string("PowerScope ")+POWERSCOPE_VERSION+" — Power Monitor");set_default_size(960,710);set_border_width(10);add(root);
  std::error_code ec;for(auto& e:fs::directory_iterator("/sys/class/power_supply",ec))if(readstr(e.path()/"type")=="Battery"){battery=e.path();break;}
  // Find a readable Intel RAPL package energy counter. Do not require root.
  fs::path base="/sys/class/powercap";
  if(fs::exists(base,ec)) {
   for(const auto& e:fs::directory_iterator(base,ec)) {
    if(readstr(e.path()/"name").rfind("package-",0)!=0)continue;
    const auto candidate=e.path()/"energy_uj";
    if(number(candidate)>=0){rapl=e.path();break;}
   }
  }
  title.set_halign(Gtk::ALIGN_START);
  title.set_markup("<span size='x-large' weight='bold'>PowerScope</span>");
  heading.pack_start(title,Gtk::PACK_EXPAND_WIDGET);
  about_button.signal_clicked().connect([this](){
   Gtk::AboutDialog dialog;
   dialog.set_transient_for(*this);
   dialog.set_program_name("PowerScope");
   dialog.set_version(POWERSCOPE_VERSION);
   dialog.set_comments("Native Linux battery, CPU, GPU and energy monitor.\nPower and process energy estimates are not direct per-device measurements.");
   dialog.set_logo_icon_name("org.powerscope.monitor");
   dialog.run();
  });
  heading.pack_end(about_button,Gtk::PACK_SHRINK);
  root.pack_start(heading,Gtk::PACK_SHRINK);
  auto setup_grid=[](Gtk::Frame& frame,Gtk::Grid& grid){
   frame.set_shadow_type(Gtk::SHADOW_ETCHED_IN);
   frame.set_hexpand(true);
   grid.set_row_spacing(5);
   grid.set_column_spacing(10);
   grid.set_margin_start(10);grid.set_margin_end(10);
   grid.set_margin_top(8);grid.set_margin_bottom(8);
   frame.add(grid);
  };
  setup_grid(battery_frame,battery_grid);
  setup_grid(system_frame,system_grid);
  setup_grid(energy_frame,energy_grid);
  auto add_field=[](Gtk::Grid& grid,int row,const char* name,Gtk::Label& value){
   auto* label=Gtk::manage(new Gtk::Label(name));
   label->set_halign(Gtk::ALIGN_START);
   label->set_xalign(0.0);
   value.set_halign(Gtk::ALIGN_END);
   value.set_xalign(1.0);
   value.set_selectable(true);
   grid.attach(*label,0,row,1,1);
   grid.attach(value,1,row,1,1);
  };
  add_field(battery_grid,0,"Charge",battery_pct);
  add_field(battery_grid,1,"Energy",battery_energy);
  add_field(battery_grid,2,"Time remaining",battery_time);
  add_field(battery_grid,3,"Battery power",battery_direction);
  add_field(system_grid,0,"CPU",cpu_value);
  add_field(system_grid,1,"GPU (approx.)",gpu_value);
  add_field(system_grid,2,"CPU temp",temp_value);
  add_field(system_grid,3,"Processes",process_value);
  add_field(energy_grid,0,"CPU package",rapl_value);
  add_field(energy_grid,1,"Source",energy_info);
  dashboard.pack_start(battery_frame,Gtk::PACK_EXPAND_WIDGET);
  dashboard.pack_start(system_frame,Gtk::PACK_EXPAND_WIDGET);
  dashboard.pack_start(energy_frame,Gtk::PACK_EXPAND_WIDGET);
  total_box.set_halign(Gtk::ALIGN_END);
  total_box.set_valign(Gtk::ALIGN_START);
  total_box.set_margin_start(10);
  total_value.set_halign(Gtk::ALIGN_END);
  total_value.set_xalign(1.0);
  total_status.set_halign(Gtk::ALIGN_END);
  total_status.set_xalign(1.0);
  total_box.pack_start(total_value,Gtk::PACK_SHRINK);
  total_box.pack_start(total_status,Gtk::PACK_SHRINK);
  dashboard.pack_end(total_box,Gtk::PACK_SHRINK);
  root.pack_start(dashboard,Gtk::PACK_SHRINK);
  toolbar.pack_start(*Gtk::manage(new Gtk::Label("Graph range:")),Gtk::PACK_SHRINK);
  for(auto n:{"1 min","5 min","15 min","30 min","60 min"}) { zoom.append(n); }
  zoom.set_active(1);
  zoom.signal_changed().connect([this](){static const int ranges[]={60,300,900,1800,3600};int i=zoom.get_active_row_number();if(i>=0&&i<5){graph.window=ranges[i];graph.queue_draw();}});
  toolbar.pack_start(zoom,Gtk::PACK_SHRINK);root.pack_start(toolbar,Gtk::PACK_SHRINK);
  graph.set_size_request(-1,290);root.pack_start(graph,Gtk::PACK_SHRINK);
  const char* names[]={"Power (W)","Battery (%)","CPU (%)","GPU (%)","Temp (°C)"};
  for(int i=0;i<5;++i){
   auto* check=Gtk::manage(new Gtk::CheckButton(names[i]));
   // Match the checkbox label to the corresponding graph line color.
   static const char* colors[]={"#4DDC9E","#66ABFF","#FFAB4F","#C785FF","#FF5C61"};
   check->get_child()->set_name("powerscope-legend-label");
   auto css=Gtk::CssProvider::create();
   css->load_from_data(std::string("#powerscope-legend-label { color: ")+colors[i]+"; }");
   check->get_child()->get_style_context()->add_provider(css,GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
   legend_checks[i]=check;
   check->set_active(true);
   check->signal_toggled().connect([this,check,i](){graph.visible[i]=check->get_active();graph.queue_draw();});
   toggles.pack_start(*check,Gtk::PACK_SHRINK);
  }
  root.pack_start(toggles,Gtk::PACK_SHRINK);
  auto* axes=Gtk::manage(new Gtk::Label("Axes: left = battery discharge W  |  right = CPU/GPU/Battery %  |  far right = °C (0–110)"));
  axes->set_halign(Gtk::ALIGN_START);root.pack_start(*axes,Gtk::PACK_SHRINK);
  note.set_halign(Gtk::ALIGN_START);note.set_line_wrap(true);note.set_text("The large value at right is net battery charging/discharging power, not adapter input power. Charging time is an estimate based on the current net battery charging rate and will change as charging slows. The Power graph shows battery discharge only. Estimated W = Intel RAPL CPU package power allocated by process CPU time; not actual process watts. N/A when RAPL is unavailable. CPU % (cores): 100% = one fully busy logical CPU, so multi-threaded processes can exceed 100%. CPU % (total) = cores % / logical CPU count. GPU % is estimated from DRM fdinfo engine busy times; unavailable clients may be omitted. GPU watts are not measured. Energy (J) integrates this estimate since PowerScope started, only while the process is visible; it resets on restart and excludes unmeasured intervals.");root.pack_start(note,Gtk::PACK_SHRINK);
  model=Gtk::ListStore::create(cols);tree.set_model(model);tree.append_column("PID",cols.pid);tree.append_column("Process",cols.name);
  tree.get_column(0)->set_sort_column(cols.pid);
  tree.get_column(1)->set_sort_column(cols.name);
  auto add_num=[this](const char* name,Gtk::TreeModelColumn<double>& field){auto* col=Gtk::manage(new Gtk::TreeViewColumn(name));auto* cell=Gtk::manage(new Gtk::CellRendererText());col->pack_start(*cell);auto* field_ptr=&field;col->set_cell_data_func(*cell,[cell,field_ptr](Gtk::CellRenderer*,const Gtk::TreeModel::const_iterator& it){cell->property_text()=fmt((*it)[*field_ptr],2);});col->set_sort_column(field);tree.append_column(*col);};
  add_num("CPU % (cores)",cols.cpu);add_num("CPU % (total)",cols.cpu_total);add_num("GPU % (approx.)",cols.gpu);add_num("RSS MiB",cols.mem);add_num("Estimated W (CPU)",cols.estimated);
  add_num("Energy (J, session)",cols.energy_j);
  model->set_sort_column(cols.cpu, Gtk::SORT_DESCENDING);
  scroll.add(tree);scroll.set_policy(Gtk::POLICY_AUTOMATIC,Gtk::POLICY_AUTOMATIC);root.pack_start(scroll,Gtk::PACK_EXPAND_WIDGET);
  load_settings();
  show_all_children();refresh();Glib::signal_timeout().connect(sigc::mem_fun(*this,&Window::refresh),1000);
 }
~Window() override {save_settings();}
private:
 void load_settings(){
  Glib::KeyFile key;
  try{
   key.load_from_file(settings_file.string());
   int w=key.get_integer("Window","width"),h=key.get_integer("Window","height");
   if(w>=600&&w<=6000&&h>=450&&h<=4000)set_default_size(w,h);
  }catch(const Glib::Error&){}
  try{int z=key.get_integer("Graph","zoom");if(z>=0&&z<5)zoom.set_active(z);}catch(const Glib::Error&){}
  for(int i=0;i<5;++i){try{legend_checks[i]->set_active(key.get_boolean("Graph","visible"+std::to_string(i)));}catch(const Glib::Error&){}}
 }
 void save_settings(){
  Glib::KeyFile key;int w=0,h=0;get_size(w,h);
  key.set_integer("Window","width",w);key.set_integer("Window","height",h);
  key.set_integer("Graph","zoom",zoom.get_active_row_number());
  for(int i=0;i<5;++i)key.set_boolean("Graph","visible"+std::to_string(i),legend_checks[i]->get_active());
  std::error_code ec;fs::create_directories(settings_file.parent_path(),ec);
  if(ec)return;
  std::ofstream f(settings_file);if(f)f<<key.to_data();
 }
 bool refresh(){auto now=std::chrono::steady_clock::now();double dt=last.time_since_epoch().count()?std::chrono::duration<double>(now-last).count():0;
  auto ct=cpu_ticks();double cpu=-1;if(lastcpu.first&&ct.first>lastcpu.first){auto total=ct.first-lastcpu.first;auto idle=ct.second-lastcpu.second;cpu=100.*(total-idle)/total;}lastcpu=ct;
  double power=-1,percent=-1,energy=-1,full=-1;std::string state="Unknown";
  if(!battery.empty()){state=readstr(battery/"status");power=number(battery/"power_now")/1e6;if(power<0){double current=number(battery/"current_now")/1e6,volt=number(battery/"voltage_now")/1e6;power=(current>=0&&volt>=0)?current*volt:-1;}
   energy=number(battery/"energy_now")/1e6;full=number(battery/"energy_full")/1e6;
   if(energy<0){double charge=number(battery/"charge_now")/1e6,volt=number(battery/"voltage_now")/1e6;energy=(charge>=0&&volt>=0)?charge*volt:-1;}
   percent=number(battery/"capacity");}
  // A charging battery's power_now is charging power, not system consumption.
  double discharge=state=="Discharging"?power:-1;
  double charging=state=="Charging"?power:-1;
  // The battery controller reports net battery power, not AC adapter input power.
  // A full battery may report "Not charging" while the adapter powers the system.
  
  double rapl_energy=rapl.empty()?-1:number(rapl/"energy_uj");package_watts=-1;
  if(dt>0&&last_rapl>=0&&rapl_energy>=0){double delta=rapl_energy-last_rapl;if(delta<0){double max=number(rapl/"max_energy_range_uj");if(max>0)delta+=max;}if(delta>=0)package_watts=delta/1e6/dt;}
  last_rapl=rapl_energy;
  struct Row{int pid;std::string name;double cpu,mem,watts,gpu;};std::vector<Row> rows;std::map<int,Proc> current;double sumcpu=0;std::error_code ec; std::map<std::string,double> gpu_engine_busy; bool gpu_supported=false;
  for(auto& e:fs::directory_iterator("/proc",ec)){auto s=e.path().filename().string();if(!digits(s))continue;int pid;try{pid=std::stoi(s);}catch(...){continue;}Proc p;if(!proc_read(pid,p))continue;
   double pcpu=0;auto old=previous.find(pid);if(old!=previous.end()&&old->second.start==p.start&&p.ticks>=old->second.ticks&&dt>.1&&hz>0)pcpu=100.*(p.ticks-old->second.ticks)/(hz*dt);
   gpu_read(pid,p);
   double pgpu=-1;
   if(!p.gpu_ns.empty()) {
    gpu_supported=true; pgpu=0;
    if(old!=previous.end()&&old->second.start==p.start&&dt>.1) {
     for(const auto& [engine,ns]:p.gpu_ns) {
      auto prev=old->second.gpu_ns.find(engine);
      if(prev!=old->second.gpu_ns.end()&&ns>=prev->second) {
       double busy=100.*double(ns-prev->second)/1e9/dt;
       pgpu+=busy;gpu_engine_busy[engine]+=busy;
      }
     }
    }
    pgpu=std::clamp(pgpu,0.,100.);
   }
   sumcpu+=pcpu;rows.push_back({pid,p.name,pcpu,p.rss*double(page)/1048576.,-1,pgpu});current.emplace(pid,std::move(p));}
  // Remove old counters when a PID disappears or is reused by another process.
  for(auto it=accumulated_j.begin();it!=accumulated_j.end();){
   auto old=previous.find(it->first), cur=current.find(it->first);
   if(cur==current.end() || old==previous.end() || old->second.start!=cur->second.start)
    it=accumulated_j.erase(it);
   else ++it;
  }
  previous.swap(current);last=now;
  // Maximum engine utilization avoids adding parallel render/video engines together.
  // It is an approximation based on visible DRM clients, not a hardware-wide meter.
  double gpu=-1;if(gpu_supported){gpu=0;for(auto& [engine,busy]:gpu_engine_busy)gpu=std::max(gpu,busy);gpu=std::clamp(gpu,0.,100.);}
  double temperature=cpu_temperature();
  graph.data.push_back({discharge,percent,cpu,gpu,temperature});if(graph.data.size()>3600)graph.data.pop_front();graph.queue_draw();
  // Allocation is explicitly an estimate, and only CPU package power.
  if(package_watts>=0&&sumcpu>0)
   for(auto& r:rows)r.watts=package_watts*r.cpu/sumcpu;
  // Integrate estimated process power over actual elapsed sample time.
  // A first sample or missing RAPL reading does not contribute energy.
  if(dt>0 && dt<30 && package_watts>=0 && sumcpu>0)
   for(const auto& r:rows)accumulated_j[r.pid]+=r.watts*dt;
  model->clear();for(auto& r:rows){auto it=model->append();(*it)[cols.pid]=r.pid;(*it)[cols.name]=r.name;(*it)[cols.cpu]=r.cpu;(*it)[cols.cpu_total]=r.cpu/logical_cpus;(*it)[cols.gpu]=r.gpu;(*it)[cols.mem]=r.mem;(*it)[cols.estimated]=r.watts;
   (*it)[cols.energy_j]=accumulated_j.count(r.pid)?accumulated_j[r.pid]:-1;}
  auto duration=[](double hours)->std::string {
   if(!std::isfinite(hours)||hours<0||hours>10000)return "N/A";
   auto minutes=static_cast<long long>(std::ceil(hours*60.));
   return std::to_string(minutes/60)+" h "+std::to_string(minutes%60)+" min";
  };
  std::string headline, remaining="N/A", status;
  if(state=="Discharging") {
   status="Discharging";
   headline=fmt(discharge,2)+" W";
   if(discharge>0 && energy>=0)remaining=duration(energy/discharge);
  } else if(state=="Charging") {
   status="Charging";
   headline="+"+fmt(charging,2)+" W";
   if(charging>0 && full>=energy && energy>=0)
    remaining=duration((full-energy)/charging);
  } else if(state=="Full") {
   status="Battery full";headline="100%";remaining="0 h 0 min";
  } else if(state=="Not charging") {
   status="Plugged in / not charging";headline="0 W";
  } else {
   status=state;headline="N/A";
  }
  total_value.set_markup("<span size='xx-large' weight='bold'>"+
   Glib::Markup::escape_text(headline)+"</span>");
  total_status.set_text(status);
  battery_pct.set_text(fmt(percent,0)+" %");
  battery_energy.set_text(fmt(energy,2)+" / "+fmt(full,2)+" Wh");
  battery_time.set_text(remaining);
  battery_direction.set_text(state=="Charging" ? "+"+fmt(charging,2)+" W" :
    (state=="Discharging" ? "−"+fmt(discharge,2)+" W" : "—"));
  cpu_value.set_text(fmt(cpu,1)+" %");
  gpu_value.set_text(fmt(gpu,1)+" %");
  temp_value.set_text(fmt(temperature,1)+" °C");
  process_value.set_text(std::to_string(rows.size()));
  rapl_value.set_text(fmt(package_watts,2)+" W");
  energy_info.set_text(rapl.empty()?"RAPL unavailable":"Intel RAPL");
  // The headline is net battery charge/discharge power, not adapter input power.
  return true;
 }
};
int main(int argc,char** argv){auto app=Gtk::Application::create(argc,argv,"org.powerscope.monitor");Window window;return app->run(window);}
