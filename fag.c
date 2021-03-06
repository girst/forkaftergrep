/* forkaftergrep (C) 2017 Tobias Girstmair, GPLv3 */
//TODO: grep is missing `-e' and `-f' options

#define _XOPEN_SOURCE 500
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#include "fag.h"

pid_t cpid = 0;

void term_child(int s) {
	if (cpid != 0) kill (cpid, s);
	exit(1);
}

int main (int argc, char** argv) {
	struct opt opts = {0, 0, 0, NULL, NULL, "/dev/null", "/dev/null", STDOUT_FILENO, "-q"};
	/* `-q': don't print anything; exit with 0 on match; with 1 on error. used to interface with `grep' */
	int opt;
	opterr = 0;

	/* generate grep options string */
	char* p = opts.grepopt+2; /* move cursor behind `q' */

	signal (SIGPIPE, SIG_IGN); /* ignore broken pipe between fag and grep */
	struct sigaction sa = {0}; /* terminate child if fag gets interrupted/terminated */
	sa.sa_handler = &term_child; /* NOTE: will also be inherited by keep-pipe-alive-child */
	sigaction (SIGINT, &sa, NULL);
	sigaction (SIGTERM, &sa, NULL);


	/* the `+' forces getopt to stop at the first non-option */
	while ((opt = getopt (argc, argv, "+t:k::rl:L:VhvEFGPiwxyUZJe:f:")) != -1) {
		switch (opt) {
		case 't':
			opts.timeout = atoi (optarg);
			break;
		case 'k':
			opts.kill_sig = optarg ? atoi (optarg) : SIGTERM;
			break;
		case 'r':
			opts.stream = STDERR_FILENO;
			break;
		case 'l':
			opts.logout = optarg;
			break;
		case 'L':
			opts.logerr = optarg;
			break;
		case 'V':
			opts.verbose = 1;
			break;
		case 'h':
			fprintf (stderr, VERTEXT USAGE
				"Options:\n"
				"\t-t N\ttimeout after N seconds\n"
				"\t-k[N]\tsend signal N to child after timeout (default: 15/SIGTERM)\n"
				"\t-r\tgrep on stderr instead of stdout\n"
				"\t-l FILE\tlog PROGRAM's stdout to FILE\n"
				"\t-L FILE\tlog PROGRAM's stderr to FILE\n"
				"\t-V\tbe verbose; print monitored stream to stderr\n"
				"\t-[EFGP]\tgrep: matcher selection\n"
				"\t-[iwxU]\tgrep: matching control\n"
				"\t-[ZJ]\tgrep: decompression control\n", argv[0]);
			return EX_OK;
		case 'v':
			fprintf (stderr, VERTEXT);
			return EX_OK;
		case 'e': case 'f':
			fprintf (stderr, "`grep -e' and `-f' are not implemented yet!\n");
			return EX_SOFTWARE;
			break;
		case 'E': case 'F': case 'G': case 'P':
		case 'i': case 'y': case 'w': case 'x':
		case 'U': case 'Z': case 'J':
			*(p++)=opt;
			break;

		default: 
			fprintf (stderr, "Unrecognized option: %c\n" USAGE, optopt, argv[0]);
			return EX_USAGE;
		}
	}
	*p = '\0'; /* terminate grep_options string */

	/* the first non-option argument is the search string */
	if (optind < argc) {
		opts.pattern = argv[optind++];
	} else {
		fprintf (stderr, USAGE "(Missing PATTERN)\n", argv[0]);
		return EX_USAGE;
	}

	/* the remaining are the program to be run */
	if (optind < argc) {
		opts.argv = &(argv[optind]);
	} else {
		fprintf (stderr, USAGE "(Missing PROGRAM)\n", argv[0]);
		return EX_USAGE;
	}

	int retval = fork_after_grep (opts);

	return retval;
}

int fork_after_grep (struct opt opts) {
	int pipefd[2];
	int pipefd_cpid[2]; /* IPC to extract PID from daemonized userprog */
	pid_t tmp_cpid;
	int status;

	int primary_logfile;
	int secondary_logfile;

	char buf[BUF_SIZE];
	int nbytes;

	struct timeval begin, now, diff;

	if (pipe(pipefd) == -1) {
		fprintf (stderr, "pipe error (userprog)\n");
		return EX_OSERR;
	}

	if(pipe(pipefd_cpid) == -1) {
		fprintf (stderr, "pipe error (cpid-ipc)\n");
		close (pipefd[0]);
		close (pipefd[1]);
		return EX_OSERR;
	}

	/* Writing the -l and -L log files is a bit messy. We can easily just pipe the stream we aren't monitoring
	into the secondary_logfile, but we can't do that for the primary_logfile. This one we write to from the piping
	process, where -V is also handled at and in the keep-pipe-alive-daemon. */
	primary_logfile = open((opts.stream==STDOUT_FILENO?opts.logout:opts.logerr), O_WRONLY|O_APPEND|O_CREAT, 0600);
	if (primary_logfile == -1) {
		fprintf (stderr, "error opening logfile (stdout): %s\n", strerror(errno));
		close (pipefd[0]);
		close (pipefd[1]);
		close (pipefd_cpid[0]);
		close (pipefd_cpid[1]);
		return EX_OSFILE;
	}
	secondary_logfile = open((opts.stream==STDOUT_FILENO?opts.logerr:opts.logout), O_WRONLY|O_APPEND|O_CREAT, 0600);
	if (secondary_logfile == -1) {
		fprintf (stderr, "error opening logfile (stderr): %s\n", strerror(errno));
		close (pipefd[0]);
		close (pipefd[1]);
		close (pipefd_cpid[0]);
		close (pipefd_cpid[1]);
		close (primary_logfile);
		return EX_OSFILE;
	}

	if ((tmp_cpid = fork()) == -1) {
		fprintf (stderr, "fork error (daemonizer): %s", strerror (errno));
		close (pipefd[0]);
		close (pipefd[1]);
		close (pipefd_cpid[0]);
		close (pipefd_cpid[1]);
		close (primary_logfile);
		close (secondary_logfile);
		return EX_OSERR;
	}

	if (tmp_cpid == 0) {
		pid_t cpid_userprog;

		close (pipefd[0]);
		dup2 (pipefd[1], opts.stream);
		close (pipefd[1]);
		dup2 (secondary_logfile, opts.stream==STDOUT_FILENO?STDERR_FILENO:STDOUT_FILENO);
		close (primary_logfile);
		close (secondary_logfile);

		if (setsid () == -1) {
			fprintf (stderr, "setsid error (daemonizer): %s", strerror (errno));
			_exit (EX_OSERR);
		}

		if ((cpid_userprog = fork()) == -1) {
			fprintf (stderr, "fork error (userprog): %s", strerror (errno));
			_exit (EX_OSERR);
		}
		if (cpid_userprog == 0) {
			close(pipefd_cpid[0]);
			close(pipefd_cpid[1]);

			execvp (opts.argv[0], opts.argv);
			fprintf (stderr, "exec error (userprog): %s", strerror (errno));
		} else {
			/* only way to get final child's pid to main is through IPC */
			write(pipefd_cpid[1], &cpid_userprog, sizeof(pid_t));
			close(pipefd_cpid[0]);
			close(pipefd_cpid[1]);
		}
		_exit (EX_UNAVAILABLE);
	} else {
		pid_t grep_cpid;
		int grep_pipefd[2];
		int grep_status;

		/* read userprog's PID from IPC: */
		read(pipefd_cpid[0], &cpid, sizeof(pid_t));
		close(pipefd_cpid[0]);
		close(pipefd_cpid[1]);

		close (pipefd[1]);
		close (secondary_logfile);
		fcntl (pipefd[0], F_SETFL, fcntl (pipefd[0], F_GETFL, 0) | O_NONBLOCK);

		gettimeofday (&begin, NULL); /* for timeout */

		if (pipe(grep_pipefd) == -1) {
			fprintf (stderr, "pipe error (grep)\n");
			close (pipefd[0]);
			return EX_OSERR;
		}

		if ((grep_cpid = fork()) == -1) {
			fprintf (stderr, "fork error (grep): %s", strerror (errno));
			close (pipefd[0]);
			close (grep_pipefd[0]);
			close (grep_pipefd[1]);
			return EX_OSERR;
		}

		if (grep_cpid == 0) {
			close (grep_pipefd[1]);
			dup2 (grep_pipefd[0], STDIN_FILENO);
			close (grep_pipefd[0]);

			close (STDOUT_FILENO);

			char* grep_override = getenv("GREP_OVERRIDE");
			if (grep_override != NULL) {
				execlp (grep_override, basename(grep_override), opts.grepopt, "--", opts.pattern, NULL);
			} else {
				execlp ("grep", "grep", opts.grepopt, "--", opts.pattern, NULL);
			}
			fprintf (stderr, "exec error (grep): %s", strerror (errno));
			_exit (EX_SOFTWARE);
		} else {
			close (grep_pipefd[0]);
			for (;;) {
				usleep (20000);
				nbytes = read (pipefd[0], buf, BUF_SIZE);
				if (nbytes == -1) {
					switch (errno) {
					case EAGAIN:
						break;
					default:
						fprintf (stderr, "read error (userprog): %s", strerror (errno));
						close (pipefd[0]);
						close (grep_pipefd[1]);
						kill (grep_cpid, SIGTERM);
						return EX_IOERR;
					}
				} else if (nbytes == 0) {
					fprintf (stderr, "Child program exited prematurely (userprog).\n");
					close (pipefd[0]);
					close (grep_pipefd[1]);
					kill (grep_cpid, SIGTERM);
					if (waitpid (cpid, &status, WNOHANG) > 0 && WIFEXITED (status)) {
						return WEXITSTATUS (status);
					}
					return EX_UNAVAILABLE;
				} else {
					/* have new userprog-data, send it to grep */
					if (opts.verbose) {
						write(STDERR_FILENO, buf, nbytes);
					}

					write(primary_logfile, buf, nbytes);

					write(grep_pipefd[1], buf, nbytes); /* can cause SIGPIPE if grep exited, therefore signal will be ignored */
				}

				if (waitpid (grep_cpid, &grep_status, WNOHANG) > 0 && WIFEXITED (grep_status)) {
					close (grep_pipefd[1]);

					if (WEXITSTATUS(grep_status) == 0) {
						/* grep exited with match found */
						printf ("%d\n", cpid);
						fflush (stdout);

						/* create a new child to keep pipe alive, empty it and write log files (will exit with exec'd program) */
						if (!fork()){setsid();if(!fork()) {
							close(0); close(1); close(2); umask(0); chdir("/");
							for (;;) {
								usleep (20000);
								nbytes = read (pipefd[0], buf, BUF_SIZE);

								if ((nbytes == -1 && errno != EAGAIN) || nbytes == 0) break;

								write(primary_logfile, buf, nbytes);
							}
							close (pipefd[0]);
							close (primary_logfile);
							_exit(0);
						}}
						close (pipefd[0]);
						close (primary_logfile);
						return EX_OK;
					} else {
						/* grep exited due to an error */
						fprintf (stderr, "grep exited due to an error.\n");
						close (pipefd[0]);
						close (grep_pipefd[1]);
						close (primary_logfile);
						return EX_IOERR;
					}
				}

				if (opts.timeout > 0) {
					gettimeofday (&now, NULL);
					timersub (&now, &begin, &diff);
					if (diff.tv_sec >= opts.timeout) {
						fprintf (stderr, "Timeout reached. \n");
						printf ("%d\n", cpid);
						fflush (stdout);

						if (opts.kill_sig > 0) kill (cpid, opts.kill_sig);

						close (pipefd[0]);
						close (grep_pipefd[1]);
						close (primary_logfile);
						kill (grep_cpid, SIGTERM);
						return EX_UNAVAILABLE;
					}
				}
			}
		}
	}
}
