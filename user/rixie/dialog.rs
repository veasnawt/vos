//! Fixed, offline replies. No model, command execution, allocation or I/O here.
pub const BANNER:&[u8]=b"Rixie Core v0 - deterministic offline prototype\nI use predefined replies, not a conversational AI model.\nType help for topics; exit ends this session.\n\n";
pub struct Reply {pub text:&'static[u8],pub exit:bool}
fn say(text:&'static[u8])->Reply {Reply{text,exit:false}}
fn trim(s:&[u8])->&[u8] {
    let mut a=0;let mut b=s.len();
    while a<b && s[a].is_ascii_whitespace() {a+=1;}
    while b>a && s[b-1].is_ascii_whitespace() {b-=1;}
    &s[a..b]
}
const HELP:&[u8]=b"Topics: hello, help, about, questions, explain <command>, exit\nExplain vsh commands: help, clear, echo, pwd, ls, cd, cat, exit\nExplanations are text only; Rixie does not execute shell commands.\n";
const ABOUT:&[u8]=b"Rixie Core v0 is an independent Rust ring-3 application in VOS.\nI am a deterministic prototype with no conversational AI model or network.\n";
const QUESTIONS:&[u8]=b"Try: what is VOS? / are you AI? / are you online?\nOr: what can you do? / who made you?\n";

pub fn respond(input:&[u8])->Reply {
    let input=trim(input);
    if input.is_empty() {return say(b"");}
    if input.eq_ignore_ascii_case(b"exit") {return Reply{text:b"Goodbye from Rixie.\n",exit:true};}
    if input.eq_ignore_ascii_case(b"hello") || input.eq_ignore_ascii_case(b"hi") {
        return say(b"Hello! I'm Rixie, at home in VOS.\n");
    }
    if input.eq_ignore_ascii_case(b"help") {return say(HELP);}
    if input.eq_ignore_ascii_case(b"about") {return say(ABOUT);}
    if input.eq_ignore_ascii_case(b"questions") {return say(QUESTIONS);}
    let split=input.iter().position(u8::is_ascii_whitespace).unwrap_or(input.len());
    if input[..split].eq_ignore_ascii_case(b"explain") {
        let command=trim(&input[split..]);
        for (name,text) in EXPLANATIONS {
            if command.eq_ignore_ascii_case(name) {return say(text);}
        }
        return say(b"No explanation for that topic. Use explain followed by a listed vsh command.\n");
    }
    let question=trim(input.strip_suffix(b"?").unwrap_or(input));
    for (name,text) in ANSWERS {
        if question.eq_ignore_ascii_case(name) {return say(text);}
    }
    say(b"No predefined reply. Type help or questions.\n")
}

const EXPLANATIONS:&[(&[u8],&[u8])]=&[
    (b"help",b"help: list vsh's supported commands.\n"),
    (b"clear",b"clear: clear vsh's display.\n"),
    (b"echo",b"echo: print text in vsh.\n"),
    (b"pwd",b"pwd: show the directory inside vsh's granted namespace.\n"),
    (b"ls",b"ls: list a directory in the read-only filesystem.\n"),
    (b"cd",b"cd: change vsh's working directory; invalid paths leave it unchanged.\n"),
    (b"cat",b"cat: display a real read-only file under vsh's grant.\n"),
    (b"exit",b"exit: leave the current native application.\n"),
];
const ANSWERS:&[(&[u8],&[u8])]=&[
    (b"what is vos",b"VOS is an independently written operating-system project, currently verified in QEMU.\n"),
    (b"are you ai",b"No. Rixie Core v0 selects fixed replies; I am not a conversational AI model.\n"),
    (b"are you online",b"No. This prototype is entirely offline, with no network or cloud connection.\n"),
    (b"what can you do",b"I greet you, explain VOS shell commands and answer a small set of predefined questions.\n"),
    (b"who made you",b"VOS is developed under human direction with AI coding assistance. Rixie v0 is native Rust code.\n"),
];

#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn greetings_and_empty() {
        assert_eq!(respond(b"  HeLLo\t").text,respond(b"hi").text);
        assert!(respond(b" \t\r\n").text.is_empty());assert!(!respond(b"").exit);
    }
    #[test] fn identity_and_help() {
        assert!(core::str::from_utf8(BANNER).unwrap().contains("not a conversational AI model"));
        assert_eq!(respond(b"HELP").text,HELP);assert_eq!(respond(b"about").text,ABOUT);
        assert_eq!(respond(b"questions").text,QUESTIONS);
    }
    #[test] fn command_explanations() {
        for (name,text) in EXPLANATIONS {
            let mut line=[0u8;32];line[..8].copy_from_slice(b"explain ");line[8..8+name.len()].copy_from_slice(name);
            let reply=respond(&line[..8+name.len()]);assert_eq!(reply.text,*text);assert!(!reply.exit);
        }
        assert_eq!(respond(b"explain   CD").text,EXPLANATIONS[5].1);
        assert!(respond(b"explain").text.starts_with(b"No explanation"));
        assert!(respond(b"explain network").text.starts_with(b"No explanation"));
    }
    #[test] fn predefined_questions() {
        for (question,text) in ANSWERS {
            assert_eq!(respond(question).text,*text);
            let mut line=[0u8;64];line[..question.len()].copy_from_slice(question);line[question.len()]=b'?';
            assert_eq!(respond(&line[..question.len()+1]).text,*text);
        }
        assert_eq!(respond(b"  ARE YOU AI?  ").text,ANSWERS[1].1);
    }
    #[test] fn unknown_and_bounds() {
        let fallback=respond(b"unknown").text;
        assert_eq!(respond(&[b'x';128]).text,fallback);assert_eq!(respond(&[0xff;128]).text,fallback);
        assert_eq!(respond(b"cd /").text,fallback);assert_eq!(respond(b"vsh rixie").text,fallback);
    }
    #[test] fn exit_is_explicit() {
        assert!(respond(b" EXIT ").exit);assert!(!respond(b"exit now").exit);
        assert!(!respond(b"explain exit").exit);assert!(!respond(b"what is vos?").exit);
    }
}
