/*
 * Simple Unix-style command shell usable in interactive or script mode.
 *
 * Copyright (C) 1997 Massachusetts Institute of Technology
 * See section "MIT License" in the file LICENSES for licensing terms.
 *
 * Derived from the MIT Exokernel and JOS.
 * Adapted for PIOS by Bryan Ford at Yale University.
 */

#include <inc/cdefs.h>
#include <inc/stdio.h>
#include <inc/stdlib.h>
#include <inc/unistd.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/errno.h>
#include <inc/args.h>
// For cd
#include <inc/stat.h>
#include <inc/file.h>
#include <inc/dirent.h>

#define BUFSIZ 1024		/* Find the buffer overrun bug! */

int debug = 0;

// gettoken(s, 0) prepares gettoken for subsequent calls and returns 0.
// gettoken(0, token) parses a shell token from the previously set string,
// null-terminates that token, stores the token pointer in '*token',
// and returns a token ID (0, '<', '>', '|', or 'w').
// Subsequent calls to 'gettoken(0, token)' will return subsequent
// tokens from the string.
int gettoken(char *s, char **token);


// Parse a shell command from string 's' and execute it.
// Do not return until the shell command is finished.
// runcmd() is called in a forked child,
// so it's OK to manipulate file descriptor state.
#define MAXARGS 256
void gcc_noreturn
runcmd(char* s)
{
	char *argv[MAXARGS], *t, argv0buf[BUFSIZ];
	int argc, c, i, r, p[2], fd, pipe_child;

	pipe_child = 0;
	if(debug)
		cprintf("runcmd: str: %s\n", s);
	gettoken(s, 0);
	
again:
	argc = 0;
	while (1) {
		char tok;
		int flags;
		switch ((c = gettoken(0, &t))) {

		case 'w':	// Add an argument
			if (argc == MAXARGS) {
				cprintf("sh: too many arguments\n");
				exit(EXIT_FAILURE);
			}
			argv[argc++] = t;
			break;
			
		case '<':	// Input redirection
			// Grab the filename from the argument list
			if (gettoken(0, &t) != 'w') {
				cprintf("syntax error: < not followed by word\n");
				exit(EXIT_FAILURE);
			}
			if ((fd = open(t, O_RDONLY)) < 0) {
				cprintf("open %s for read: %e", t, fd);
				exit(EXIT_FAILURE);
			}
			if (fd != 0) {
				dup2(fd, 0);
				close(fd);
			}
			break;
			
		case '>':	// Output redirection
			// Grab the filename from the argument list
			tok = gettoken(0, &t);
			flags = O_WRONLY | O_CREAT | O_TRUNC;
			if (tok == '>') {
				flags = O_WRONLY | O_CREAT | O_APPEND;
				tok = gettoken(0, &t);
			}
			if (tok != 'w') {
				cprintf("syntax error: > not followed by word\n");
				exit(EXIT_FAILURE);
			}
			if ((fd = open(t, flags)) < 0) {
				cprintf("open %s for write: %s", t,
					strerror(errno));
				exit(EXIT_FAILURE);
			}
			if (fd != 1) {
				dup2(fd, 1);
				close(fd);
			}
			break;
			
		case 0:		// String is complete
			// Run the current command!
			goto runit;
			
		default:
			panic("bad return %d from gettoken", c);
			break;
			
		}
	}

runit:
	// Return immediately if command line was empty.
	if(argc == 0) {
		if (debug)
			cprintf("EMPTY COMMAND\n");
		exit(EXIT_SUCCESS);
	}

	// Clean up command line.
	// Read all commands from the filesystem: add an initial '/' to
	// the command name.
	// This essentially acts like 'PATH=/'.
	if (argv[0][0] != '/') {
		argv0buf[0] = '/';
		strcpy(argv0buf + 1, argv[0]);
		argv[0] = argv0buf;
	}
	argv[argc] = 0;

	// Print the command.
	if (debug) {
		cprintf("execv:");
		for (i = 0; argv[i]; i++)
			cprintf(" %s", argv[i]);
		cprintf("\n");
	}
	if (execv(argv[0], argv) < 0)
		cprintf("exec %s: %s\n", argv[0], strerror(errno));
	exit(EXIT_FAILURE);
}


// Get the next token from string s.
// Set *p1 to the beginning of the token and *p2 just past the token.
// Returns
//	0 for end-of-string;
//	< for <;
//	> for >;
//	| for |;
//	w for a word.
//
// Eventually (once we parse the space where the \0 will go),
// words get nul-terminated.
#define WHITESPACE " \t\r\n"
#define SYMBOLS "<|>&;()"

int
_gettoken(char *s, char **p1, char **p2)
{
	int t;

	if (s == 0) {
		if (debug > 1)
			cprintf("GETTOKEN NULL\n");
		return 0;
	}

	if (debug > 1)
		cprintf("GETTOKEN: %s\n", s);

	*p1 = 0;
	*p2 = 0;

	while (*s && strchr(WHITESPACE, *s))
		*s++ = 0;
	if (*s == 0) {
		if (debug > 1)
			cprintf("EOL\n");
		return 0;
	}
	if (strchr(SYMBOLS, *s)) {
		t = *s;
		*p1 = s;
		*s++ = 0;
		*p2 = s;
		if (debug > 1)
			cprintf("TOK %c\n", t);
		return t;
	}
	*p1 = s;
	while (*s && !strchr(WHITESPACE SYMBOLS, *s))
		s++;
	*p2 = s;
	if (debug > 1) {
		t = **p2;
		**p2 = 0;
		cprintf("WORD: %s\n", *p1);
		**p2 = t;
	}
	return 'w';
}

int
gettoken(char *s, char **p1)
{
	static int c, nc;
	static char* np1, *np2;

	if (s) {
		nc = _gettoken(s, &np1, &np2);
		return 0;
	}
	c = nc;
	*p1 = np1;
	nc = _gettoken(np2, &np1, &np2);
	return c;
}


void
usage(void)
{
	cprintf("usage: sh [-dix] [command-file]\n");
	exit(EXIT_FAILURE);
}

int
main(int argc, char **argv)
{
	int r, interactive, echocmds, clear = 0;

	interactive = '?';
	echocmds = 0;
	ARGBEGIN{
	case 'd':
		debug++;
		break;
	case 'i':
		interactive = 1;
		break;
	case 'x':
		echocmds = 1;
		break;
	default:
		usage();
	}ARGEND

	if (argc > 1)
		usage();
	if (argc == 1) {
		close(0);
		if ((r = open(argv[0], O_RDONLY)) < 0)
			panic("open %s: %s", argv[0], strerror(errno));
		assert(r == 0);
	}
	if (interactive == '?')
		interactive = isatty(0);

	while (1) {
		char *buf;
		if(clear) {
			clear = 0;
			int a;
			for(a=0; a < 40; a++) {
				printf("\n\n");
			}
			continue;
		}
		buf = readline(interactive ? "$ " : NULL);
		if (buf == NULL) {
			if (debug)
				cprintf("EXITING\n");
			exit(EXIT_SUCCESS);	// end of file
		}
		if (buf[0] == 0) {
			continue; // Empty line
		}
		if (debug)
			cprintf("LINE: %s\n", buf);
		if (buf[0] == '#')
			continue;
		if (echocmds)
			fprintf(stdout, "# %s\n", buf);
		// Prepare to parse built-in commands
		char shBuf[strlen(buf)+1];
		strcpy(shBuf, buf);
		if(debug)
			cprintf("sh: copy done (%s, %s)\n", buf, shBuf);
		char *token;
		gettoken(shBuf, 0);
		if(debug)
			cprintf("sh: set token shBuf\n");
		gettoken(0, &token);				// Get command name
		if(debug)
			cprintf("TOKEN: %s|\n", token);
		if (!strcmp(token, "exit"))	// built-in command
			exit(0);
		if (!strcmp(token, "cwd")) {
			printf("%s\n", files->fi[files->cwd].de.d_name);
			continue;
		}
		if (!strcmp(token, "pwd")) {
			char* paths[32];
			char output[32];
			int num_paths = 0;
			int ino = files->cwd;
			// Trace back paths until root
			while(ino != FILEINO_ROOTDIR) {
				paths[num_paths++] = files->fi[ino].de.d_name;
				if(debug)
					cprintf("pwd: storing %s index %d for ino %d\n", 
						files->fi[ino].de.d_name, num_paths-1, ino);
				ino = files->fi[ino].dino;
			}
			if(!num_paths) {
				printf("/\n");
				continue;
			}
			int len = 0;
			// Print them out in reverse order
			for(num_paths--; num_paths >= 0; num_paths--) {
				char *path = paths[num_paths];
				int plen = strlen(path)+1;
				if(debug)
					cprintf("pwd: num_path: %d, len: %d, plen: %d, path: %s, dest %x", 
						num_paths, len, plen, path, output+len, output);
				snprintf(&(output[len]), plen+1, "/%s", path); // +1 for slash, +2 for \0
				len += plen;
			}
			printf("%s\n", output);
			continue;
		}
		if (!strcmp(token, "cd")) {
			char *dir;
			int res;
			gettoken(0, &dir);				// Get directory to change to
			if(dir) {
				res = dir_walk(dir, 0);	// Get inode number
			} else {
				res = dir_walk("/", 0);	// Default to "/"
			}
			if(res == -1) {
				fprintf(stderr, "cd: directory not found\n");
			} else {
				assert(res > 2);				// Not one of the reserved inodes
				if(fileino_isdir(res)) {
					files->cwd = res;			// Change directory
				} else {
					fprintf(stderr, "cd: %s is not a directory", dir);
				}

			}
			continue;
		}
		if (!strcmp(token, "clear")) {
			clear = 1;
		}
		if (debug)
			cprintf("BEFORE FORK\n");
		if ((r = fork()) < 0)
			panic("fork: %e", r);
		if (debug)
			cprintf("FORK: %d\n", r);
		if (r == 0) {
			runcmd(buf);
			exit(EXIT_SUCCESS);
		} else
			waitpid(r, NULL, 0);
	}
}

