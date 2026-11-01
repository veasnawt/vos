use console_line_core::{Storage,take_line,PENDING,NOSPACE,TOOLONG};
#[derive(Clone)]
struct Model { ready: bool, overflow: bool, len: u32, cap: u32, present: bool,
    data: [u8; 300], offset: usize, cleared: bool, writes: usize }
impl Default for Model { fn default()->Self { let mut data=[0;300]; for (i,b) in data.iter_mut().enumerate(){*b=i as u8;} Self{ready:true,overflow:false,len:2,cap:129,present:true,data,offset:150,cleared:false,writes:0} } }
impl Storage for Model {
 fn ready(&self)->bool{self.ready} fn overflow(&self)->bool{self.overflow}
 fn length(&self)->u32{self.len} fn capacity(&self)->u32{self.cap} fn output_valid(&self,_n:u32)->bool{self.present}
 fn read(&self,i:u32)->u8{assert!(i<=128); self.data[i as usize]}
 fn write(&mut self,i:u32,b:u8){self.data[self.offset+i as usize]=b;self.writes+=1;}
 fn clear(&mut self){self.ready=false;self.len=0;self.overflow=false;self.cleared=true;}
}
#[test] fn pending_precedence(){let mut s=Model{ready:false,overflow:true,..Model::default()};assert_eq!(take_line(&mut s),PENDING);assert!(!s.cleared);assert_eq!(s.writes,0);}
#[test] fn overflow_reset(){let mut s=Model{overflow:true,present:false,cap:0,..Model::default()};assert_eq!(take_line(&mut s),TOOLONG);assert!(s.cleared);assert_eq!(s.writes,0);}
#[test] fn capacity_equality(){let mut s=Model{len:2,cap:2,..Model::default()};assert_eq!(take_line(&mut s),NOSPACE);assert!(!s.cleared);assert_eq!(s.writes,0);}
#[test] fn null_output(){let mut s=Model{present:false,..Model::default()};assert_eq!(take_line(&mut s),NOSPACE);assert!(!s.cleared);}
#[test] fn zero_length(){let mut s=Model{len:0,..Model::default()};assert_eq!(take_line(&mut s),0);assert_eq!(s.writes,1);assert_eq!(s.data[150],0);assert!(s.cleared && !s.ready);assert_eq!(take_line(&mut s),PENDING);}
#[test] fn includes_terminator(){let mut s=Model::default();assert_eq!(take_line(&mut s),2);assert_eq!(&s.data[150..153],&[0,1,2]);assert!(s.cleared);}
#[test] fn maximum(){let mut s=Model{len:128,..Model::default()};assert_eq!(take_line(&mut s),128);assert_eq!(s.writes,129);assert_eq!(s.data[278],128);}
#[test] fn invalid_length(){let mut s=Model{len:129,cap:u32::MAX,..Model::default()};assert_eq!(take_line(&mut s),NOSPACE);assert!(!s.cleared);assert_eq!(s.writes,0);}
#[test] fn forward_overlap(){let mut s=Model{len:8,offset:1,..Model::default()};assert_eq!(take_line(&mut s),8);assert_eq!(&s.data[0..10],&[0;10]);}
#[test] fn repeated_reset(){let mut s=Model::default();assert_eq!(take_line(&mut s),2);assert_eq!(take_line(&mut s),PENDING);assert_eq!(s.writes,3);}
