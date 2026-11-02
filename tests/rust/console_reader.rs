use console_reader_core::{read_line,Reader};
struct M {k:bool,out:bool,cap:u32,initialized:bool,calls:u32,cancel_at:u32,polls:u32,pending:u32,takes:usize,results:Vec<i32>,log:Vec<u8>}
impl Default for M {fn default()->Self{Self{k:true,out:true,cap:129,initialized:false,calls:0,cancel_at:0,polls:0,pending:0,takes:0,results:vec![2],log:vec![]}}}
impl Reader for M {
 fn keyboard_ready(&self)->bool{self.k} fn output_present(&self)->bool{self.out} fn capacity(&self)->u32{self.cap}
 fn initialize(&mut self){self.initialized=true;self.log.push(b'I');}
 fn prompt(&mut self){assert!(self.initialized);self.log.push(b'P');}
 fn cancelled(&mut self)->bool{self.calls+=1;self.log.push(b'C');self.cancel_at!=0&&self.calls>=self.cancel_at}
 fn poll_accept(&mut self){assert!(self.initialized);self.polls+=1;assert!(self.polls<10);self.log.push(b'K');}
 fn line_ready(&self)->bool{self.polls>self.pending}
 fn take_line(&mut self)->i32{self.log.push(b'T');let r=self.results[self.takes];self.takes+=1;r}
 fn overlong(&mut self){self.log.push(b'O');}
 fn sleep(&mut self){self.log.push(b'S');}
}
#[test]fn unavailable_precedence(){let mut m=M{k:false,out:false,..M::default()};assert_eq!(read_line(&mut m),-4);assert!(m.log.is_empty());}
#[test]fn null_output(){let mut m=M{out:false,..M::default()};assert_eq!(read_line(&mut m),-2);assert!(m.log.is_empty());}
#[test]fn capacity_boundary(){let mut m=M{cap:128,..M::default()};assert_eq!(read_line(&mut m),-2);assert!(m.log.is_empty());}
#[test]fn initialization(){let mut m=M::default();assert_eq!(read_line(&mut m),2);assert!(m.initialized);}
#[test]fn prompt_before_cancel(){let mut m=M{cancel_at:1,..M::default()};assert_eq!(read_line(&mut m),-5);assert_eq!(m.log,b"IPC");}
#[test]fn cancel_before_poll(){let mut m=M{cancel_at:1,..M::default()};assert_eq!(read_line(&mut m),-5);assert_eq!(m.polls,0);}
#[test]fn success_no_sleep(){let mut m=M::default();assert_eq!(read_line(&mut m),2);assert_eq!(m.log,b"IPCKT");}
#[test]fn pending_wait(){let mut m=M{pending:2,..M::default()};assert_eq!(read_line(&mut m),2);assert_eq!(m.log,b"IPCKSCKSCKT");}
#[test]fn overflow_recovery(){let mut m=M{results:vec![-3,3],..M::default()};assert_eq!(read_line(&mut m),3);assert_eq!(m.log,b"IPCKTOPSCKT");}
#[test]fn error_propagation(){let mut m=M{results:vec![-2],..M::default()};assert_eq!(read_line(&mut m),-2);assert_eq!(m.log,b"IPCKT");}
#[test]fn empty_success(){let mut m=M{results:vec![0],..M::default()};assert_eq!(read_line(&mut m),0);assert_eq!(m.takes,1);}
#[test]fn mid_wait_cancel(){let mut m=M{pending:5,cancel_at:3,..M::default()};assert_eq!(read_line(&mut m),-5);assert_eq!(m.log,b"IPCKSCKSC");}
