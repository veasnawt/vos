//! Format the shell's normalized working directory; no command history or heap.
pub const PATH_MAX:usize=256;
pub const PROMPT_MAX:usize=4+PATH_MAX+2; // VOS>, path, final > and space

pub fn format(cwd:&[u8],out:&mut[u8;PROMPT_MAX])->usize {
    assert!(cwd.len()<=PATH_MAX);
    out[..4].copy_from_slice(b"VOS>");
    let mut n=4;
    for &c in cwd {out[n]=if c==b'/' {b'>'} else {c};n+=1;}
    if !cwd.is_empty() {out[n]=b'>';n+=1;}
    out[n]=b' ';n+1
}

/* Resolve inside the explicitly granted root. The shell commits this candidate
 * as cwd only after opening it successfully through the existing file API. */
pub fn resolve(cwd:&[u8],arg:&[u8],out:&mut[u8;256])->Option<usize> {
    let mut n=0;
    if !arg.starts_with(b"/") {out[..cwd.len()].copy_from_slice(cwd);n=cwd.len();}
    for p in arg.split(|c| *c==b'/') {
        if p.is_empty() || p==b"." {continue;}
        if p==b".." {while n>0 && out[n-1]!=b'/' {n-=1;}if n>0 {n-=1;}continue;}
        if n+p.len()+1>out.len() {return None;}
        if n>0 {out[n]=b'/';n+=1;}
        out[n..n+p.len()].copy_from_slice(p);n+=p.len();
    }
    Some(n)
}

#[cfg(test)]
mod tests {
    use super::*;
    fn check(cwd:&[u8],expected:&[u8]) {let mut out=[0;PROMPT_MAX];let n=format(cwd,&mut out);assert_eq!(&out[..n],expected);}
    #[test] fn requested_prompts() {
        check(b"",b"VOS> ");check(b"docs",b"VOS>docs> ");check(b"docs/results",b"VOS>docs>results> ");
    }
    #[test] fn normalized_navigation() {
        let mut path=[0;PATH_MAX];
        let n=resolve(b"",b"/docs//results/.",&mut path).unwrap();check(&path[..n],b"VOS>docs>results> ");
        let n=resolve(b"docs/results",b"..",&mut path).unwrap();check(&path[..n],b"VOS>docs> ");
        let n=resolve(b"docs/results",b"/",&mut path).unwrap();check(&path[..n],b"VOS> ");
        let n=resolve(b"docs",b"../docs/results/../results",&mut path).unwrap();check(&path[..n],b"VOS>docs>results> ");
        let n=resolve(b"docs",b"../../..",&mut path).unwrap();check(&path[..n],b"VOS> ");
        assert!(resolve(&[b'x';PATH_MAX],b"extra",&mut path).is_none());
    }
    #[test] fn maximum_prompt_is_bounded() {
        let mut guarded=[0x5Au8;PROMPT_MAX+2];let mut path=[b'x';PATH_MAX];path[127]=b'/';
        let n=format(&path,(&mut guarded[1..PROMPT_MAX+1]).try_into().unwrap());
        assert_eq!(n,PROMPT_MAX);assert_eq!(guarded[0],0x5A);assert_eq!(guarded[PROMPT_MAX+1],0x5A);
        assert_eq!(&guarded[1..5],b"VOS>");assert_eq!(guarded[1+4+127],b'>');assert_eq!(&guarded[PROMPT_MAX-1..PROMPT_MAX+1],b"> ");
    }
}
