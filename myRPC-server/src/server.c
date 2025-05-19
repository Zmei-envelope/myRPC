#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pwd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "config_parser.h"
#include "libmysyslog.h"  // Библиотека для системного логирования

#define BUFFER_SIZE 1024  // Размер буфера для чтения/записи данных

// Глобальная переменная для контроля работы сервера
volatile sig_atomic_t stop;

/**
 * Обработчик сигналов для корректного завершения работы сервера
 * @param sig Номер полученного сигнала
 */
void handle_signal(int sig) {
    stop = 1;
}

/**
 * Проверка наличия пользователя в списке разрешенных
 * @param username Имя пользователя для проверки
 * @return 1 если пользователь разрешен, 0 в противном случае
 */
int user_allowed(const char *username) {
    // Открытие файла с конфигурацией пользователей
    FILE *file = fopen("/etc/myRPC/users.conf", "r");
    if (!file) {
        mysyslog("Failed to open users.conf", ERROR, 0, 0, "/var/log/myrpc.log");
        perror("Failed to open users.conf");
        return 0;
    }

    char line[256];
    int allowed = 0;
    
    // Построчное чтение файла конфигурации
    while (fgets(line, sizeof(line), file)) {
        // Удаление символа новой строки
        line[strcspn(line, "\n")] = '\0';

        // Пропуск комментариев и пустых строк
        if (line[0] == '#' || strlen(line) == 0)
            continue;

        // Проверка соответствия имени пользователя
        if (strcmp(line, username) == 0) {
            allowed = 1;
            break;
        }
    }

    fclose(file);
    return allowed;
}

/**
 * Выполнение команды с перенаправлением вывода в файлы
 * @param command Команда для выполнения
 * @param stdout_file Файл для стандартного вывода
 * @param stderr_file Файл для вывода ошибок
 */
void execute_command(const char *command, char *stdout_file, char *stderr_file) {
    // Формирование команды с перенаправлением вывода
    char cmd[BUFFER_SIZE];
    snprintf(cmd, BUFFER_SIZE, "%s >%s 2>%s", command, stdout_file, stderr_file);
    system(cmd);
}

int main() {
    // Установка обработчиков сигналов для корректного завершения
    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    // Чтение конфигурационного файла сервера
    Config config = parse_config("/etc/myRPC/myRPC.conf");

    int port = config.port;
    int use_stream = strcmp(config.socket_type, "stream") == 0;

    // Логирование запуска сервера
    mysyslog("Server starting...", INFO, 0, 0, "/var/log/myrpc.log");

    // Создание сокета в зависимости от типа (STREAM/DGRAM)
    int sockfd;
    if (use_stream) {
        sockfd = socket(AF_INET, SOCK_STREAM, 0);
    } else {
        sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    }

    if (sockfd < 0) {
        mysyslog("Socket creation failed", ERROR, 0, 0, "/var/log/myrpc.log");
        perror("Socket creation failed");
        return 1;
    }

    // Установка параметра для повторного использования порта
    int opt = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        mysyslog("setsockopt failed", ERROR, 0, 0, "/var/log/myrpc.log");
        perror("setsockopt failed");
        close(sockfd);
        return 1;
    }

    // Настройка адреса сервера
    struct sockaddr_in servaddr, cliaddr;
    socklen_t len;
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(port);

    // Привязка сокета к адресу
    if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) {
        mysyslog("Bind failed", ERROR, 0, 0, "/var/log/myrpc.log");
        perror("Bind failed");
        close(sockfd);
        return 1;
    }

    // Для STREAM сокета - переход в режим ожидания соединений
    if (use_stream) {
        listen(sockfd, 5);
        mysyslog("Server listening (stream)", INFO, 0, 0, "/var/log/myrpc.log");
    } else {
        mysyslog("Server listening (datagram)", INFO, 0, 0, "/var/log/myrpc.log");
    }

    // Основной цикл обработки запросов
    while (!stop) {
        char buffer[BUFFER_SIZE];
        int n;

        if (use_stream) {
            // Обработка STREAM соединений
            len = sizeof(cliaddr);
            int connfd = accept(sockfd, (struct sockaddr*)&cliaddr, &len);
            if (connfd < 0) {
                mysyslog("Accept failed", ERROR, 0, 0, "/var/log/myrpc.log");
                perror("Accept failed");
                continue;
            }

            // Чтение данных от клиента
            n = recv(connfd, buffer, BUFFER_SIZE, 0);
            if (n <= 0) {
                close(connfd);
                continue;
            }
            buffer[n] = '\0';

            // Логирование полученного запроса
            mysyslog("Received request", INFO, 0, 0, "/var/log/myrpc.log");

            // Разбор полученных данных (формат: username:command)
            char *username = strtok(buffer, ":");
            char *command = strtok(NULL, "");
            if (command) {
                while (*command == ' ')
                    command++;
            }

            char response[BUFFER_SIZE];

            // Проверка прав пользователя
            if (user_allowed(username)) {
                mysyslog("User allowed", INFO, 0, 0, "/var/log/myrpc.log");

                // Создание временных файлов для вывода команды
                char stdout_file[] = "/tmp/myRPC_XXXXXX.stdout";
                char stderr_file[] = "/tmp/myRPC_XXXXXX.stderr";
                mkstemp(stdout_file);
                mkstemp(stderr_file);

                // Выполнение команды
                execute_command(command, stdout_file, stderr_file);

                // Чтение результатов выполнения
                FILE *f = fopen(stdout_file, "r");
                if (f) {
                    size_t read_bytes = fread(response, 1, BUFFER_SIZE, f);
                    response[read_bytes] = '\0';
                    fclose(f);
                    mysyslog("Command executed successfully", INFO, 0, 0, "/var/log/myrpc.log");
                } else {
                    strcpy(response, "Error reading stdout file");
                    mysyslog("Error reading stdout file", ERROR, 0, 0, "/var/log/myrpc.log");
                }

                // Удаление временных файлов
                remove(stdout_file);
                remove(stderr_file);

            } else {
                snprintf(response, BUFFER_SIZE, "1: User '%s' is not allowed", username);
                mysyslog("User not allowed", WARN, 0, 0, "/var/log/myrpc.log");
            }

            // Отправка ответа клиенту
            send(connfd, response, strlen(response), 0);
            close(connfd);

        } else {
            // Обработка DGRAM сообщений
            len = sizeof(cliaddr);
            n = recvfrom(sockfd, buffer, BUFFER_SIZE, 0, (struct sockaddr*)&cliaddr, &len);
            if (n <= 0) {
                continue;
            }
            buffer[n] = '\0';

            // Логирование полученного запроса
            mysyslog("Received request", INFO, 0, 0, "/var/log/myrpc.log");

            // Разбор полученных данных (формат: username:command)
            char *username = strtok(buffer, ":");
            char *command = strtok(NULL, "");
            if (command) {
                while (*command == ' ')
                    command++;
            }

            char response[BUFFER_SIZE];

            // Проверка прав пользователя
            if (user_allowed(username)) {
                mysyslog("User allowed", INFO, 0, 0, "/var/log/myrpc.log");

                // Создание временных файлов для вывода команды
                char stdout_file[] = "/tmp/myRPC_XXXXXX.stdout";
                char stderr_file[] = "/tmp/myRPC_XXXXXX.stderr";
                mkstemp(stdout_file);
                mkstemp(stderr_file);

                // Выполнение команды
                execute_command(command, stdout_file, stderr_file);

                // Чтение результатов выполнения
                FILE *f = fopen(stdout_file, "r");
                if (f) {
                    size_t read_bytes = fread(response, 1, BUFFER_SIZE, f);
                    response[read_bytes] = '\0';
                    fclose(f);
                    mysyslog("Command executed successfully", INFO, 0, 0, "/var/log/myrpc.log");
                } else {
                    strcpy(response, "Error reading stdout file");
                    mysyslog("Error reading stdout file", ERROR, 0, 0, "/var/log/myrpc.log");
                }

                // Удаление временных файлов
                remove(stdout_file);
                remove(stderr_file);

            } else {
                snprintf(response, BUFFER_SIZE, "1: User '%s' is not allowed", username);
                mysyslog("User not allowed", WARN, 0, 0, "/var/log/myrpc.log");
            }

            // Отправка ответа клиенту
            sendto(sockfd, response, strlen(response), 0, (struct sockaddr*)&cliaddr, len);
        }
    }

    // Завершение работы сервера
    close(sockfd);
    mysyslog("Server stopped", INFO, 0, 0, "/var/log/myrpc.log");
    return 0;
}
