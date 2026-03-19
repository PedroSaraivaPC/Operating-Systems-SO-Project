#include "common.h"

int nveiculos_max = 10;  // variavel que vai receber o valor que está guardado na variável ambiente, caso a variavel nao tenha sido definida, por defeito, o máx é 10
int fd_sv; // descriptor do pipe público. necessario ser variavel global para 'encerrar_sistema'

// Antes usávamos: 'pthread_cancel' na thread do client e admin, enquanto os whiles eram infinitos ("while(1)"), o que faria com que se matasse as threads de forma bruta, deixando ficheiros abertos e/ou memória por limpar
// Por conselho do professor Filipe Sá, usamos sinais (SIGUSR1) para interromper as chamadas bloqueantes (como o 'read'), isto apenas na thead do admin e do cliente.
// Assim, as threads "acordam" com o erro EINTR, verificam que esta flag 'sistema_ativo' passou a 0, e saem dos seus loops ordeiramente, garantindo a limpeza correta dos recursos
int sistema_ativo = 1; // flag de controlo de ciclo de vida (1=Running, 0=Shutdown)

// IDs das Threads Principais (Globais para poder enviar sinais de desbloqueio): thread do admin e clientes
pthread_t tid_admin, tid_clientes;


// --- gestão de utilizadores (protegida por mutex) --- 
typedef struct {
    char username[MAX_USERNAME];
    int pid;
} ClienteAtivo;
ClienteAtivo utilizadores_ativos[MAX_UTILIZADORES]; // lista de todos os utilizadores
int num_utilizadores_ativos = 0;

// Mutex 1: Protege a lista de logins
// Evita que dois clientes façam login ao mesmo tempo e ocupem o mesmo slot.
pthread_mutex_t mutex_utilizadores = PTHREAD_MUTEX_INITIALIZER;


// --- gestão de serviços e frotas (protegidas por mutex) ---
Servico lista_servicos[MAX_SERVICOS]; // lista de todos os serviços
int num_servicos_agendados = 0;
int total_km_percorridos = 0; // estistica global, para saber o n.º de km percorridos por todos os clientes

// Mutex 2: Protege a lista de serviços
// O relógio lê/escreve aqui, a telemetria escreve aqui, o admin lê aqui. Sem este mutex, teria-se Race Conditions graves nos estados das viagens.
pthread_mutex_t mutex_servicos = PTHREAD_MUTEX_INITIALIZER;

// Relógio Simulado
int hora_simulada = 0;
pthread_t tid_relogio; // ID da thread do relogio

// Lista das viagens que estão a decorrer neste momento (EM_VIAGEM)
FrotaAtiva frota_ativa[10];
int num_veiculos_ativos = 0;


// --- funcoes auxiliares ---
// Converte ENUM em String para os logs do Admin ficarem legíveis
char* get_estado_str(EstadoServico estado) {
    switch (estado) {
        case AGENDADO: return "AGENDADO";
        case EM_VIAGEM: return "EM VIAGEM";
        case CANCELADO: return "CANCELADO";
        case CONCLUIDO: return "CONCLUÍDO";
        default: return "DESCONHECIDO";
    }
}

// Handler vazio para SIGUSR1. Apenas interrompe chamadas bloqueantes (read/pause) com o erro: 'EINTR'.
void handler_desbloqueio(int sig) {
    // Não faz nada intencionalmente.
}

// encerramento do sistema, seja por CTRL+C ou comando 'terminar' do admin
void encerrar_sistema(int sig) {
    printf("\n[CONTROLADOR-SV] A iniciar encerramento geral...\n");
    
    sistema_ativo = 0; // passa a flag para 0 para os loops pararem

    // desbloqueia as threads presas no read (admin e cliente). Envia-se SIGUSR1 para forçar o read() a retornar -1 (EINTR)
    if (tid_admin != 0) {
        pthread_kill(tid_admin, SIGUSR1);
    }
    if (tid_clientes != 0) {
        pthread_kill(tid_clientes, SIGUSR1);
    }
    
    // fecha e apaga o pipe público (ninguém mais entra)
    close(fd_sv);
    unlink(FIFO_CONTROLADOR); // remove o ficheiro
    
    // avisa e expulsa os clientes ativos
    pthread_mutex_lock(&mutex_utilizadores);
    printf("[CONTROLADOR-SV] A avisar %d clientes...\n", num_utilizadores_ativos);
    
    MsgControladorCliente msg_fim;
    sprintf(msg_fim.mensagem, "Servidor a desligar.."); // a mensagem que o cliente espera
    msg_fim.sucesso = 0;

    for (int i = 0; i < num_utilizadores_ativos; i++) { // para cada um dos clientes ativos
        char fifo_cli[100];
        sprintf(fifo_cli, FIFO_CLIENTE, utilizadores_ativos[i].pid); // fifo_cli fica com: "./tmp/cli_" + "3152"
        
        // tenta abrir o pipe privado de cada cliente e envia a mensagem final de encerramento 
        int fd = open(fifo_cli, O_WRONLY | O_NONBLOCK);
        if (fd != -1) {
            write(fd, &msg_fim, sizeof(MsgControladorCliente));
            close(fd);
        }
    }
    pthread_mutex_unlock(&mutex_utilizadores);

    // termina os veículos que estão atualmente em viagem 
    pthread_mutex_lock(&mutex_servicos); // usamos este mutex para a frota atual (veiculos a serem usados atualmente)
    printf("[CONTROLADOR-SV] A terminar %d veiculos...\n", num_veiculos_ativos);
    
    for (int i = 0; i < num_veiculos_ativos; i++) { // para cada um dos veiculos ativos
        printf(" -> A matar veiculo PID %d\n", frota_ativa[i].pid_veiculo);
        // Isto desencadeia a morte do filho -> fecho do pipe anónimo -> fim da thread de telemetria.
        kill(frota_ativa[i].pid_veiculo, SIGUSR1); // Envia sinal de fim ao processo de cada veiculo e cada thread de cada veiculo é apagada automaticamente graças ao 'pthread_detach'
    }
    pthread_mutex_unlock(&mutex_servicos);
    
    usleep(200000); // Pequena pausa para dar tempo às mensagens de saírem  
    
    pthread_mutex_destroy(&mutex_utilizadores);
    pthread_mutex_destroy(&mutex_servicos);
    
    printf("[CONTROLADOR-SV] Sistema encerrado. Adeus!\n");
    exit(0);
}

/**
 * Thread #4: Ouve a telemetria de um único veículo (A TELEMETRIA)
 */
void* thread_telemetria(void* arg) { // esta thread é criada dinamicamente pela thread do relógio. Há uma destas por cada veículo na estrada
    FrotaAtiva *veiculo_info = (FrotaAtiva*)arg; // O pthread_create só aceita void*, mas nós passámos um ponteiro para 'FrotaAtiva'. Aqui recuperamos a estrutura para saber QUEM é o veículo e QUAL o pipe dele.
    char buffer[200]; // vai gurdar os dados que vêm do pipe anonimo do veiculo
    int n; // quant em bytes do q se vai ler
    
    printf("[TELEMETRIA %d] A ouvir veículo ID %d (PID %d)...\n", veiculo_info->servico_id, veiculo_info->servico_id, veiculo_info->pid_veiculo);
    printf("\n >>> ");
    fflush(stdout);  

    // variaveis locais para o extrair os dados das msgs
    int km, percent;
    int id_servico_array_index = veiculo_info->servico_id - 1; // saca a posição do serviço na lista (o seu servico id - 1)

    // read fica bloqueado a ler do pipe anonimo, à esperaq o veiculo escreva algo
    while ((n = read(veiculo_info->fd_leitura_anony_pipe, buffer, sizeof(buffer) - 1)) > 0) { // apenas se ler +0 bytes é q entra no while
        buffer[n] = '\0'; // ultima posicao do array ficha '\0' (string valida)
        
        char *linha = strtok(buffer, "\n"); // o strtok quebra linhas quando encontra um '\n'. Isto porque mensagens de progresso e embarque por exemplo podem vir muito rapidamente do pipe.., e pode vir tudo junto, mas com o strtok, é separado em linhas diferentes

        while (linha != NULL) { // enquanto houver linhas a ler no buffer
            
            // quando é mensagem de progresso de viagem
            if (sscanf(linha, "[VEICULO %*d] PROGRESSO | %d%% | KM: %d", &percent, &km) == 2) { // supondo que a viagem tem 10 km, está nos 30%, km: 3
                
                pthread_mutex_lock(&mutex_servicos); // como vamos mexer na lista dos serviços, temos que dar lock no mutex
                
                int km_neste_ciclo = km - lista_servicos[id_servico_array_index].distancia_percorrida; // km_neste_ciclo = 3 - 2(distancia que ja for percorrida, neste caso é 2 que nos 20% foram 2 km) = 1 | Isto significa que neste ciclo, andou 1 km
                total_km_percorridos += km_neste_ciclo; // n.º total de km percorridos pela frota += 1
                
                lista_servicos[id_servico_array_index].distancia_percorrida = km; // distancia percorrida é o km do progresso atual: 3
                
                // guarda o PID do cliente para lhe enviar tambem o progresso, desta forma, o mesmo fica ciente do decorrer da sua viagem
                int pid_cliente_destino = lista_servicos[id_servico_array_index].pid_cliente;
                
                printf("[TELEMETRIA %d] PROGRESSO: %d%%, Km: %d. Total KM (Sistema): %d", veiculo_info->servico_id, percent, km, total_km_percorridos);
                printf("\n >>> ");
                fflush(stdout);                        
                       
                pthread_mutex_unlock(&mutex_servicos); // como ja nao se vai mexer mais na lista dos serviços, faz-se unlock ao mutex
                
                // envio do progresso ao cliente
                char fifo_cli_nome[100];
                sprintf(fifo_cli_nome, FIFO_CLIENTE, pid_cliente_destino); // fifo_cli_nome fica com: "./tmp/cli_" + "3152"
                int fd_cli_prog = open(fifo_cli_nome, O_WRONLY | O_NONBLOCK); // Abre o pipe do cliente (NONBLOCK para não encravar se o cliente tiver saído)
                if (fd_cli_prog != -1) {
                    MsgControladorCliente msg_prog;
                    msg_prog.sucesso = 1; // Info normal
                    sprintf(msg_prog.mensagem, "A sua viagem esta a decorrer: %d%% concluido (%d km).", percent, km);
                    
                    write(fd_cli_prog, &msg_prog, sizeof(MsgControladorCliente));
                    close(fd_cli_prog);
                }
            }
            // quando é mensagem de embarque do cliente
            else if (strstr(linha, "EMBARQUE") != NULL) {
                printf("[TELEMETRIA %d] Cliente embarcou. Viagem iniciada.", veiculo_info->servico_id);
                printf("\n >>> ");
                fflush(stdout);
                
                pthread_mutex_lock(&mutex_servicos);
                int pid_cliente_destino = lista_servicos[id_servico_array_index].pid_cliente;
                pthread_mutex_unlock(&mutex_servicos);
               
                char fifo_cli_nome[100];
                sprintf(fifo_cli_nome, FIFO_CLIENTE, pid_cliente_destino); // fifo_cli_nome fica com: "./tmp/cli_" + "3152"
                int fd_cli_fim = open(fifo_cli_nome, O_WRONLY | O_NONBLOCK);
                if (fd_cli_fim != -1) {
                    MsgControladorCliente msg_fim;
                    msg_fim.sucesso = 1;
                    sprintf(msg_fim.mensagem, "Viagem COMEÇOU! Desfrute da paisagem.");
                    write(fd_cli_fim, &msg_fim, sizeof(MsgControladorCliente));
                    close(fd_cli_fim);
                }
            }
            // quando o cliente sai antes do fim da viagem
            else if (strstr(linha, "CLIENTE_SAIU") != NULL) {
                int km_final;
                // Tenta extrair o KM onde saiu: "[VEICULO X] CLIENTE_SAIU | O cliente saiu no KM: %d.". Se não conseguir ler, assume-se os kms atuais.
                if (sscanf(linha, "[VEICULO %*d] CLIENTE_SAIU | O cliente saiu no KM: %d", &km_final) == 1) { // supondo que saiu no km 6
                    // Atualiza KMs finais
                    pthread_mutex_lock(&mutex_servicos);
                    
                    int km_neste_ciclo = km_final - lista_servicos[id_servico_array_index].distancia_percorrida; // km_neste_ciclo = 6 - 5 (caso seja uma viagem de 10km) = 1
                    total_km_percorridos += km_neste_ciclo; // distancia total da frota += 1
                    lista_servicos[id_servico_array_index].distancia_percorrida = km_final; // distancia percorrida é o km que ele saiu
                    
                    pthread_mutex_unlock(&mutex_servicos);
                }

                pthread_mutex_lock(&mutex_servicos);
                
                lista_servicos[id_servico_array_index].estado = CONCLUIDO; // mesmo quando é o cliente a sair a meio da viagem, da-se a viagem por concluida
                int pid_cliente_destino = lista_servicos[id_servico_array_index].pid_cliente; // Guardar PID
                printf("[TELEMETRIA %d] Viagem interrompida pelo cliente. Saiu no km: %d.\n", veiculo_info->servico_id, lista_servicos[id_servico_array_index].distancia_percorrida);
                
                pthread_mutex_unlock(&mutex_servicos);
                
                printf("\n >>> ");
                fflush(stdout);
                
                // avisa o cliente que a viagem terminou
                char fifo_cli_nome[100];
                sprintf(fifo_cli_nome, FIFO_CLIENTE, pid_cliente_destino);
                int fd_cli_fim = open(fifo_cli_nome, O_WRONLY | O_NONBLOCK); // O_NONBLOCK para não encravar se o cliente tiver saído
                if (fd_cli_fim != -1) {
                    MsgControladorCliente msg_fim;
                    msg_fim.sucesso = 1;
                    sprintf(msg_fim.mensagem, "Viagem terminada, saiu mais cedo no km: %d. Obrigado!",lista_servicos[id_servico_array_index].distancia_percorrida);
                    write(fd_cli_fim, &msg_fim, sizeof(MsgControladorCliente));
                    close(fd_cli_fim);
                }
                                
                break;
            }
            // quando chega ao fim da viagem
            else if (strstr(linha, "FIM_VIAGEM") != NULL) {
                pthread_mutex_lock(&mutex_servicos);
                
                lista_servicos[id_servico_array_index].estado = CONCLUIDO;
                int pid_cliente_destino = lista_servicos[id_servico_array_index].pid_cliente;
                printf("[TELEMETRIA %d] Serviço CONCLUÍDO (Chegada ao destino).\n", veiculo_info->servico_id);
                printf("\n >>> ");
                fflush(stdout);
                
                pthread_mutex_unlock(&mutex_servicos);
                
                // avisa-se o cliente que chegou ao destino
                char fifo_cli_nome[100];
                sprintf(fifo_cli_nome, FIFO_CLIENTE, pid_cliente_destino);
                int fd_cli_fim = open(fifo_cli_nome, O_WRONLY | O_NONBLOCK);
                if (fd_cli_fim != -1) {
                    MsgControladorCliente msg_fim;
                    msg_fim.sucesso = 1;
                    sprintf(msg_fim.mensagem, "CHEGOU AO DESTINO! Viagem concluida.");
                    write(fd_cli_fim, &msg_fim, sizeof(MsgControladorCliente));
                    close(fd_cli_fim);
                }
            }
            // apenas para receber esta mensagem do veiculo quando o admin cancela, de forma a esta mensagem não ir para o 'else' (linha nao reconhecida)
            else if (strstr(linha, "RECEBI CANCELAMENTO") != NULL) {
                printf("[TELEMETRIA %d] Veículo confirmou cancelelamento da viagem por SIGUSR1, vai avisar o cliente e terminar.\n", veiculo_info->servico_id);
                printf("\n >>> ");
                fflush(stdout); 

            }
            // também serve apenas para ignorar a mensagem do inicio do serviço e para não entrar no 'else' (linha nao reconhecida)
            else if (strstr(linha, "START") != NULL) {
                 
            }
            // caso seja outra qql linha nao reconhecida
            else {
                 // Debug apenas se não for uma linha vazia
                 if (strlen(linha) > 1)
                    printf("[DEBUG] Linha não reconhecida: '%s'\n", linha);
                    printf("\n >>> ");
                    fflush(stdout);
            }

            // avança para a próxima linha no buffer
            linha = strtok(NULL, "\n");
        }
    }
    
    // O read() devolveu 0 ou -1 (ceículo terminou ou o pipe fechou-se)
    printf("[TELEMETRIA %d] Veículo PID %d desconectou-se. A fechar pipe.\n", veiculo_info->servico_id, veiculo_info->pid_veiculo);   
    printf("\n >>> ");
    fflush(stdout); 

    close(veiculo_info->fd_leitura_anony_pipe); // fecha o pipe anonimo

    pthread_mutex_lock(&mutex_servicos);
    // remove este veículo da lista "FrotaAtiva" (para o Relógio saber que há +1 vaga)  
    int i;
    for (i = 0; i < num_veiculos_ativos; i++) {
        if (frota_ativa[i].servico_id == veiculo_info->servico_id) {
             // Move o último elemento para a posição i
             frota_ativa[i] = frota_ativa[num_veiculos_ativos - 1];
             num_veiculos_ativos--;
             break;
        }
    }
    pthread_mutex_unlock(&mutex_servicos);
    
    return NULL; // return NULL mata a thread de forma limpa. Como se fez 'pthread_detach' no relógio, a memória é libertada automaticamente
}

/**
 * Thread #3: Simula o tempo (O Relógio)
 */
void* thread_relogio(void* arg) {
    // loop infinito até o programa ser morto com exit()
    while(1) {
        sleep(1); // Simula 1 segundo de tempo real = 1 unidade de tempo simulado
        
        pthread_mutex_lock(&mutex_servicos);
        
        hora_simulada++; // Incrementa a hora simulada
        // printf("[RELOGIO] Hora: %d\n", hora_simulada); // escreve 1 em 1s na consola a hora
        
        // percorre a lista de todos os serviços agendados para ver se é preciso lançar algum
        for (int i = 0; i < num_servicos_agendados; i++) {
            Servico *s = &lista_servicos[i];
            
            if (s->estado == AGENDADO && s->hora_inicio == hora_simulada) { // verifica se está na hora de lançar algum serviço (Estado AGENDADO e hora certa)
                
                // verifica se nao passa o limite de carros permitidos ao mesmo tempo
                if (num_veiculos_ativos >= nveiculos_max) {
                     printf("[RELOGIO] ERRO: Frota cheia. Serviço %d cancelado.\n", s->id);
                     s->estado = CANCELADO;
                     
                     // Informa-se o cliente que a sua viagem foii cancelada
                     char fifo_cli[100];
                     sprintf(fifo_cli, FIFO_CLIENTE, s->pid_cliente);
                     int fd_aviso = open(fifo_cli, O_WRONLY | O_NONBLOCK); // NONBLOCK para o relógio não encravar se o cliente já tiver ido embora
                     if (fd_aviso != -1) {
                         MsgControladorCliente msg_recusa;
                         msg_recusa.sucesso = 0;
                         sprintf(msg_recusa.mensagem, "PEDIDO RECUSADO: A frota está cheia! O seu servico com id: %d foi cancelado.", s->id);
                         write(fd_aviso, &msg_recusa, sizeof(MsgControladorCliente));
                         close(fd_aviso);
                     }
                     
                     continue;
                }
                
                // cria o Pipe Anónimo (exclusivo para a thread da telemetria (veiculo))
                int cano_telemetria[2]; // cano_telemetria[0]=leitura (Pai), cano_telemetria[1]=escrita (Filho/Veículo)
                if (pipe(cano_telemetria) == -1) {
                    perror("[RELOGIO] Erro ao criar pipe telemetria");
                    continue;
                }
                
                // faz-se Fork para dividir em pai e filho
                pid_t pid_filho = fork(); // pid_filho = 0 -> filho | pid_filho = 3045(p.ex.) -> pai
                
                if (pid_filho == 0) { // codigo do filho (veículo)
                    
                    // redireciona stdout (FD 1) para o pipe anónimo de telemetria
                    close(cano_telemetria[0]); // Filho não lê
                    close(1); // Fecha o stdout normal
                    dup(cano_telemetria[1]); // FD 1 agora aponta para a ponta de escrita do pipe
                    close(cano_telemetria[1]); // Fecha a cópia original do FD
                    
                    // constrói os argumentos (6 argumentos + 1 nome = 7 strings)
                    char id_str[10], dist_str[10], pidcli_str[10];
                    sprintf(id_str, "%d", s->id);
                    sprintf(dist_str, "%d", s->distancia_km);
                    sprintf(pidcli_str, "%d", s->pid_cliente);
                    char fifo_nome_completo[100];
                    sprintf(fifo_nome_completo, FIFO_CLIENTE, s->pid_cliente);
                    
                    // executa o veículo
                    execlp("./veiculo", 
                           "veiculo", 
                           id_str, dist_str, s->local_partida, 
                           s->username_cliente, pidcli_str, 
                           fifo_nome_completo,
                           NULL);
                    
                    // Se chegar aqui, o exec falhou
                    perror("[RELOGIO] ERRO FATAL: Falha no exec do veículo");
                    exit(1); 

                } else if (pid_filho > 0) { // codigo do pai (controlador)
                    
                    // pai/controlador não usa a ponta de escrita do pipe anónimo (só lê)
                    close(cano_telemetria[1]);
                    
                    // atualiza o estado do serviço e guarda o PID/FD na Frota Ativa
                    s->estado = EM_VIAGEM;
                    s->pid_veiculo = pid_filho;
                    
                    // regista o novo veículo na frota ativa
                    FrotaAtiva *nova_frota = &frota_ativa[num_veiculos_ativos];
                    nova_frota->pid_veiculo = pid_filho;
                    nova_frota->fd_leitura_anony_pipe = cano_telemetria[0]; // guarda a ponta de leitura
                    nova_frota->servico_id = s->id;

                    // lança a thread de Telemetria dedicada para este carro
                    pthread_create(&nova_frota->tid, NULL, thread_telemetria, (void*)nova_frota);
                    pthread_detach(nova_frota->tid); // Detach: Quando acabar, limpa-se sozinha
                    
                    num_veiculos_ativos++;
                    printf("[RELOGIO] Serviço %d (Cliente %s) LANÇADO. Veículo PID %d.\n", s->id, s->username_cliente, pid_filho);
                           
                    printf("\n >>> ");
                    fflush(stdout); 
                    
                } else { // caso pid_filho == -1 (erro no fork)
                    perror("[RELOGIO] ERRO: Falha no fork");
                    s->estado = AGENDADO; // tenta de novo depois
                }
            }
        }
        pthread_mutex_unlock(&mutex_servicos);
    }
    return NULL; // mata a thread automaticamente ao retornar NULL e limpa a memoria graças ao 'pthread_detach'
}

/**
 * Thread #1: Ouve comandos do Administrador (stdin)
 */
void* thread_admin_listener(void* arg) {
    char buffer[200];
    int n;
    printf("[ADMIN] Thread de admin pronta. Escreva comandos:\n");
    printf("\n >>> ");
    fflush(stdout); 
    
    while(sistema_ativo) { // verifica a flag global. Se sistema_ativo = 0 (shutdown), o loop termina.
        n = read(STDIN_FILENO, buffer, sizeof(buffer)-1);
        // Bloqueia aqui, à espera de comandos no stdin
        if (n > 0) { // se ler algum byte do stdin
            buffer[n] = '\0';
            buffer[strcspn(buffer, "\n")] = 0; // Tira o '\n' (ENTER)

            if (strcmp(buffer, "utiliz") == 0) { // caso o que o user escreva seja exatamente igual a "utiliz" (comando)
                pthread_mutex_lock(&mutex_utilizadores); // Protege a lista
                
                printf("\n--- Utilizadores Ativos (%d/%d) ---\n", num_utilizadores_ativos, MAX_UTILIZADORES);
                for (int i = 0; i < num_utilizadores_ativos; i++) {
                    printf("  -> %s (PID: %d)\n", utilizadores_ativos[i].username, utilizadores_ativos[i].pid);
                }
                printf("----------------------------------\n");
                
                pthread_mutex_unlock(&mutex_utilizadores); // Liberta a lista
            }
            else if (strcmp(buffer, "terminar") == 0) {
                printf("[ADMIN] Comando 'terminar' recebido. A iniciar shutdown...\n");
                               
                sistema_ativo = 0; // passa a flag para 0 -> shutdown
                pthread_kill(tid_clientes, SIGUSR1); // acorda a outra thread dos clientes que está a "dormir" no read()
                
                break; // Sai do loop da thread admin
            }
            else if (strcmp(buffer, "hora") == 0) {
                pthread_mutex_lock(&mutex_servicos);
                printf("[ADMIN] Hora atual: %d\n", hora_simulada);
                pthread_mutex_unlock(&mutex_servicos);
            }
            else if (strcmp(buffer, "listar") == 0) {
                pthread_mutex_lock(&mutex_servicos);
                printf("\n--- Serviços Agendados (%d/%d) ---\n", num_servicos_agendados, MAX_SERVICOS);
                for (int i = 0; i < num_servicos_agendados; i++) {
                     printf(" ID %d: Cliente %s | Hora: %d | Distância: %d | ESTADO: %s\n", 
                             lista_servicos[i].id, 
                             lista_servicos[i].username_cliente,
                             lista_servicos[i].hora_inicio,
                             lista_servicos[i].distancia_km,
                             get_estado_str(lista_servicos[i].estado));
                }
                printf("--------------------------------\n");
                pthread_mutex_unlock(&mutex_servicos);
            }
            else if (strcmp(buffer, "km") == 0) {
                pthread_mutex_lock(&mutex_servicos);
                printf("[ADMIN] Total de KM percorridos por toda a frota: %d km\n", total_km_percorridos);
                pthread_mutex_unlock(&mutex_servicos);
            }
            else if (strncmp(buffer, "cancelar", 8) == 0) { // primeiros 8 caracteres
                int id_cancelar;
                if (sscanf(buffer, "cancelar %d", &id_cancelar) == 1) { // guarda em 'id_cancelar' o id que está asseguir ao "cancelar"
                    
                    pthread_mutex_lock(&mutex_servicos);
                    
                    // caso 1: cancelar todos os serviços que não estão concluídos ("cancelar 0")
                    if (id_cancelar == 0) {
                        int cancelados = 0;
                        for(int i=0; i<num_servicos_agendados; i++) {
                            // Só cancela se não estiver CONCLUIDO nem já CANCELADO
                            if (lista_servicos[i].estado != CONCLUIDO && lista_servicos[i].estado != CANCELADO) {
                                if (lista_servicos[i].estado == EM_VIAGEM) { // se tiver em viagem, mata-se o veiculo
                                    kill(lista_servicos[i].pid_veiculo, SIGUSR1); // Mata veículo
                                } else if(lista_servicos[i].estado == AGENDADO){
                                    // avisa o cliente via named pipe
                                    char fifo_cli[100];
                                    sprintf(fifo_cli, FIFO_CLIENTE, lista_servicos[i].pid_cliente);
                                    int fd = open(fifo_cli, O_WRONLY | O_NONBLOCK);
                                    if (fd != -1) {
                                        MsgControladorCliente msg;
                                        msg.sucesso = 0;
                                        sprintf(msg.mensagem, "AVISO: O seu servico agendado (id: %d) foi CANCELADO pelo Admin!", i+1);
                                        write(fd, &msg, sizeof(MsgControladorCliente));
                                        close(fd);
                                    }
                                }
                                lista_servicos[i].estado = CANCELADO;
                                cancelados++;
                            }
                        }
                        printf("[ADMIN] Cancelados %d serviços ativos/agendados.\n", cancelados);
                    }
                    // caso 2: cancelar um serviço com id específico
                    else {
                        int idx = -1;
                        for(int i=0; i<num_servicos_agendados; i++) {
                            if(lista_servicos[i].id == id_cancelar) {
                                idx = i; break;
                            }
                        }

                        if (idx != -1) {
                            if (lista_servicos[idx].estado == CONCLUIDO || lista_servicos[idx].estado == CANCELADO) {
                                printf("[ADMIN] Erro: Serviço %d já não está ativo.\n", id_cancelar);
                            } else {
                                if (lista_servicos[idx].estado == EM_VIAGEM) { // caso o serviço esteja em viagem
                                    printf("[ADMIN] A interromper viagem em curso (PID %d)...\n", lista_servicos[idx].pid_veiculo);
                                    kill(lista_servicos[idx].pid_veiculo, SIGUSR1);
                                }
                                else{
                                    // avisa o cliente via named pipe
                                    char fifo_cli[100];
                                    sprintf(fifo_cli, FIFO_CLIENTE, lista_servicos[idx].pid_cliente);
                                    int fd = open(fifo_cli, O_WRONLY | O_NONBLOCK);
                                    if (fd != -1) {
                                        MsgControladorCliente msg;
                                        msg.sucesso = 0;
                                        sprintf(msg.mensagem, "AVISO: O seu servico agendado (id: %d) foi CANCELADO pelo Admin!", idx+1);
                                        write(fd, &msg, sizeof(MsgControladorCliente));
                                        close(fd);
                                    }
                                }
                                lista_servicos[idx].estado = CANCELADO;
                                printf("[ADMIN] Serviço %d cancelado.\n", id_cancelar);
                            }
                        } else {
                            printf("[ADMIN] Erro: Serviço %d não encontrado.\n", id_cancelar);
                        }
                    }
                    
                    pthread_mutex_unlock(&mutex_servicos);
                    
                } else {
                    printf("[ADMIN] Erro sintaxe: cancelar <id> (0 para todos)\n");
                }
            }
            else if (strcmp(buffer, "frota") == 0) {
                pthread_mutex_lock(&mutex_servicos);
                
                printf("\n=== ESTADO DA FROTA ===\n");
                int veiculos_em_movimento = 0;

                for (int i = 0; i < num_servicos_agendados; i++) {
                    // A frota corresponde aos serviços que estão EM_VIAGEM
                    if (lista_servicos[i].estado == EM_VIAGEM) {
                        
                        float progresso = 0.0;
                        if (lista_servicos[i].distancia_km > 0) {
                            progresso = (float)lista_servicos[i].distancia_percorrida / lista_servicos[i].distancia_km * 100.0; // calcula a percentagem da viagem
                        }

                        printf(" -> Veículo PID %d | Cliente: %s | Progresso: %.1f%% (%d/%d km)\n",
                               lista_servicos[i].pid_veiculo,
                               lista_servicos[i].username_cliente,
                               progresso,
                               lista_servicos[i].distancia_percorrida,
                               lista_servicos[i].distancia_km);
                        
                        veiculos_em_movimento++;
                    }
                }

                if (veiculos_em_movimento == 0) {
                    printf(" Nenhum veículo em movimento neste momento.\n");
                }
                
                printf("=======================\n");
                pthread_mutex_unlock(&mutex_servicos);
            }
            else if (strcmp(buffer, "clear") == 0) { // comando para apagar a consola (apenas para ficar + organizado e limpo)
                system("clear"); // usa a funcao de sistema operativo 'clear'
                printf("[ADMIN] Thread de admin pronta. Escreva comandos:\n");
                fflush(stdout); 
            }
            else {
                printf("[ADMIN] Comando '%s' desconhecido.\nApenas existem os seguintes comandos:\n\t- utiliz        -> lista todos os utilizadores\n\t- hora          -> mostra a hora atual\n\t- listar        -> lista todos os serviços\n\t- frota         -> mostra a %% da viagem de cada um dos veículos\n\t- cancelar <id> -> cancelar viagens de clientes\n\t- km            -> n.º total de km percorridos por toda a frota\n\t- terminar      -> deixar de correr o servidor\n\t- clear         -> apagar a consola\n", buffer);
            }
            printf("\n >>> ");
            fflush(stdout); 
        }
        else {
            // Se read deu erro (-1) e foi por causa do sinal SIGUSR1 (EINTR)
            if (n == -1 && errno == EINTR) continue; // volta ao while, vê sistema_ativo=0 e sai
            break;
        }
    }
    return NULL;
}


/**
 * Thread #2: Ouve pedidos dos Clientes (pipe público)
 */
void* thread_client(void* arg) {
    MsgClienteControlador msg_recebida;
    MsgControladorCliente resposta;
    char fifo_privado_nome[100];
    int fd_cli;
    
    char cmd_tipo[50];
    int hora, dist;
    char local[50];
    
    int n; // bytes do read

    printf("[CONTROLADOR-SV] Servidor pronto. A escutar em %s\n", FIFO_CONTROLADOR);

    // Loop Principal, controlado novamente pela flag
    while (sistema_ativo) {
        // Bloqueia aqui até um cliente escrever no pipe público
        n = read(fd_sv, &msg_recebida, sizeof(MsgClienteControlador)); // n fica com o nº de bytes lidos. Lê o que vem do 'fd_sv', mete na struct 'msg_recebida' e tem um tamanho da struct 'MsgClienteControlador'
        
        // tratamento do sinal de interrupção
        if (n == -1) {
            if (errno == EINTR) {
                // aqui a 'thread_client' é acordada pelo sinal de shutdown (SIGUSR1).
                // O loop vai reiniciar, verificar 'sistema_ativo' (que é 0) e terminar a thread.
                continue; 
            }
            perror("[CONTROLADOR] Erro leitura pipe");
            break;
        }
        
        if (n == sizeof(MsgClienteControlador)) { // apenas entra se foi lido a struct completa
            sprintf(fifo_privado_nome, FIFO_CLIENTE, msg_recebida.pid); // 'fifo_privado_nome' fica com FIFO_CLIENTE+PID
            
            // Tenta abrir o pipe privado para escrita
            fd_cli = open(fifo_privado_nome, O_WRONLY);
            
            if (fd_cli == -1) {
                fprintf(stderr, "[CONTROLADOR-SV] Aviso: Nao consegui abrir o pipe privado: %s\n", fifo_privado_nome);
                continue; 
            }

            // caso a mensagem do cliente seja do tipo "LOGIN"
            if (msg_recebida.tipo == TIPO_LOGIN) {
                printf("[CONTROLADOR-SV] Recebido LOGIN de '%s' (PID %d)\n", msg_recebida.username, msg_recebida.pid);
                pthread_mutex_lock(&mutex_utilizadores); // Protege a lista
                int sucesso = 1;
                
                if (num_utilizadores_ativos >= MAX_UTILIZADORES) { // Verifica se o servidor está cheio a nível de utilizadores
                    sucesso = 0;
                    sprintf(resposta.mensagem, "ERRO: Servidor cheio (Max: %d)!", MAX_UTILIZADORES);
                } else { // Verifica se o nome já existe
                    for (int i = 0; i < num_utilizadores_ativos; i++) {
                        if (strcmp(utilizadores_ativos[i].username, msg_recebida.username) == 0) {
                            sucesso = 0;
                            sprintf(resposta.mensagem, "ERRO: Utilizador '%s' já existe!", msg_recebida.username);
                            break;
                        }
                    }
                }
                
                // Se deu sucesso, adiciona à lista
                if (sucesso) {
                    strcpy(utilizadores_ativos[num_utilizadores_ativos].username, msg_recebida.username);
                    utilizadores_ativos[num_utilizadores_ativos].pid = msg_recebida.pid;
                    num_utilizadores_ativos++;
                    sprintf(resposta.mensagem, "Login feito com sucesso. Bem-vindo, %s!", msg_recebida.username);
                }
                
                pthread_mutex_unlock(&mutex_utilizadores); // Liberta a lista
                resposta.sucesso = sucesso;
                
                printf("\n >>> ");
                fflush(stdout); 
            } 
            // caso a mensagem do cliente seja um CMD normal
            else if (msg_recebida.tipo == TIPO_CMD) {
                printf("[CONTROLADOR-SV] Recebido CMD de '%s' (PID %d): '%s'\n", msg_recebida.username, msg_recebida.pid, msg_recebida.comando);
                
                // desmembra o comando (ex: "agendar 10 local_A 50")
                int num_campos_lidos = sscanf(msg_recebida.comando, "%s %d %s %d", cmd_tipo, &hora, local, &dist);

                if (strcmp(cmd_tipo, "agendar") == 0) {
                    if (num_campos_lidos == 4) {
                        pthread_mutex_lock(&mutex_servicos);
                    
                        if (hora <= hora_simulada) {
                            sprintf(resposta.mensagem, "ERRO: Nao pode agendar para o passado! (Hora atual: %d)", hora_simulada);
                            resposta.sucesso = 0;
                        }
                        else if (num_servicos_agendados < MAX_SERVICOS) {
                            Servico *s = &lista_servicos[num_servicos_agendados]; // ponteiro que vai apontar para a ultima posicao do array, para adicionar o serviço que o cliente acaba de solicitar
                            
                            s->id = num_servicos_agendados + 1;
                            s->hora_inicio = hora;
                            s->distancia_km = dist;
                            s->estado = AGENDADO; // Estado inicial
                            s->pid_cliente = msg_recebida.pid;
                            s->hora_pedido = hora_simulada;
                            strcpy(s->username_cliente, msg_recebida.username);
                            strcpy(s->local_partida, local);
                            
                            num_servicos_agendados++;

                            sprintf(resposta.mensagem, "Servico (id: %d) agendado para a hora %d. Dist: %dkm.", s->id, s->hora_inicio, s->distancia_km);
                            resposta.sucesso = 1;
                            
                        } else {
                            sprintf(resposta.mensagem, "ERRO: Limite maximo de servicos atingido (%d).", MAX_SERVICOS);
                            resposta.sucesso = 0;
                        }
                        
                        pthread_mutex_unlock(&mutex_servicos);
                    } else {
                        // erro no formato, apenas 1, 2 ou 3 campos foram lidos, mas 4 eram necessários
                        sprintf(resposta.mensagem, "ERRO: Comando 'agendar' requer 3 argumentos: agendar <hora> <local> <distancia>.");
                        resposta.sucesso = 0;
                    }
                }
                else if (strcmp(msg_recebida.comando, "terminar") == 0) {
                    
                    pthread_mutex_lock(&mutex_utilizadores);
                    pthread_mutex_lock(&mutex_servicos);
                    
                    // verifica se o cliente tem viagens a decorrer, se sim, nao deixa terminar sessao
                    int em_viagem = 0;
                    for (int k = 0; k < num_servicos_agendados; k++) {
                        if (strcmp(lista_servicos[k].username_cliente, msg_recebida.username) == 0) {
                            if (lista_servicos[k].estado == EM_VIAGEM) {
                                em_viagem = 1;
                                break;
                            }
                        }
                    }
                    
                    if (em_viagem) {
                        // Se estiver em viagem, rejeita o pedido de terminar sessao
                        sprintf(resposta.mensagem, "ERRO: Nao pode terminar enquanto tem viagens a decorrer!");
                        resposta.sucesso = 0;
                        
                    } else {
                        // cancela todos os serviços AGENDADOS (futuros) deste cliente
                        int cancelados = 0;
                        for (int k = 0; k < num_servicos_agendados; k++) {
                            if (strcmp(lista_servicos[k].username_cliente, msg_recebida.username) == 0) {
                                if (lista_servicos[k].estado == AGENDADO) {
                                    lista_servicos[k].estado = CANCELADO;
                                    cancelados++;
                                }
                            }
                        }
                        if (cancelados > 0) {
                            printf("[CONTROLADOR-SV] %d servicos agendados de '%s' foram cancelados automaticamente.\n", cancelados, msg_recebida.username);
                        }

                        // procura o user na lista de utilizadores e remove-o
                        int i, j, entrou=0;
                        for (i = 0; i < num_utilizadores_ativos; i++) {
                            if (strcmp(utilizadores_ativos[i].username, msg_recebida.username) == 0) {
                                entrou = 1;
                                // vai puxando os da direita dele, uma casa para a esquerda de forma a "tapar o buraco"
                                for (j = i; j < num_utilizadores_ativos - 1; j++) {
                                    utilizadores_ativos[j] = utilizadores_ativos[j + 1];
                                }
                                num_utilizadores_ativos--;
                                sprintf(resposta.mensagem, "Sessao terminada com sucesso. Ate logo!");
                                printf("[CONTROLADOR-SV] Utilizador '%s' removido da lista de utilizadores ativos\n", msg_recebida.username);
                                resposta.sucesso = 1;
                                break;
                            }
                        }
                        // Se o user não for encontrado
                        if (entrou!=1) {
                            sprintf(resposta.mensagem, "Aviso: Nao encontrei utilizador ativo para terminar.");
                            printf("[CONTROLADOR-SV] Aviso: utilizador '%s' não encontrado na lista ativos\n", msg_recebida.username);
                            resposta.sucesso = 0;
                        }
                    }
                    
                    pthread_mutex_unlock(&mutex_servicos);
                    pthread_mutex_unlock(&mutex_utilizadores);
                    
                }
                else if (strcmp(cmd_tipo, "cancelar") == 0) {
                    int id_cancelar;
                    if (sscanf(msg_recebida.comando, "%*s %d", &id_cancelar) == 1) {
                        
                        pthread_mutex_lock(&mutex_servicos);
                        int idx = -1;
                        for(int i=0; i<num_servicos_agendados; i++) { // procura na lista dos serviços
                            if(lista_servicos[i].id == id_cancelar) {
                                idx = i; break;
                            }
                        }

                        if (idx != -1) {
                            // verifica se é o dono desse serviço
                            if (strcmp(lista_servicos[idx].username_cliente, msg_recebida.username) != 0) {
                                sprintf(resposta.mensagem, "ERRO: Nao tem permissao para cancelar o servico %d.", id_cancelar);
                                resposta.sucesso = 0;
                            }
                            // verifica se já foi realizado (clienten ao pode cancelar se já estiver em viagem)
                            else if (lista_servicos[idx].estado == EM_VIAGEM) {
                                sprintf(resposta.mensagem, "ERRO: Viagem ja iniciada. Apenas Admin pode cancelar.");
                                resposta.sucesso = 0;
                            }
                            // verifica se já acabou
                            else if (lista_servicos[idx].estado != AGENDADO) {
                                sprintf(resposta.mensagem, "ERRO: Servico %d nao pode ser cancelado (Concluido/Ja Cancelado).", id_cancelar);
                                resposta.sucesso = 0;
                            }
                            // finalmente cancela
                            else {
                                lista_servicos[idx].estado = CANCELADO;
                                sprintf(resposta.mensagem, "Servico %d cancelado com sucesso.", id_cancelar);
                                resposta.sucesso = 1;
                            }
                        } else {
                             sprintf(resposta.mensagem, "ERRO: Servico %d nao encontrado.", id_cancelar);
                             resposta.sucesso = 0;
                        }
                        
                        pthread_mutex_unlock(&mutex_servicos);
                        
                    } else {
                        sprintf(resposta.mensagem, "ERRO: Sintaxe incorreta. Use: cancelar <id>");
                        resposta.sucesso = 0;
                    }
                }
                else if (strcmp(cmd_tipo, "consultar") == 0) {
    
                    pthread_mutex_lock(&mutex_servicos);
                    
                    char buffer_resposta[2000] = ""; // Buffer grande para a lista
                    char linha_temp[200];
                    int count = 0;

                    // Cabeçalho
                    sprintf(buffer_resposta, "\n--- Os seus servicos ---\n");

                    for (int i = 0; i < num_servicos_agendados; i++) {
                        // Verifica se o serviço pertence a este cliente
                        if (strcmp(lista_servicos[i].username_cliente, msg_recebida.username) == 0) {
                            
                            sprintf(linha_temp, "ID %d | Origem: %s | Hora: %d | Distância total: %d | Estado: %s\n", 
                                    lista_servicos[i].id, 
                                    lista_servicos[i].local_partida,
                                    lista_servicos[i].hora_inicio,
                                    lista_servicos[i].distancia_km,
                                    get_estado_str(lista_servicos[i].estado));
                            
                            // Adiciona à resposta caso a adicao dessa linha nao exceda o limite de tamanho do 'buffer_resposta'
                            if (strlen(buffer_resposta) + strlen(linha_temp) < sizeof(buffer_resposta)) {
                                strcat(buffer_resposta, linha_temp);
                            }
                            count++;
                        }
                    }

                    if (count == 0) {
                        sprintf(resposta.mensagem, "Nao tem servicos agendados.");
                    } else {
                        strncpy(resposta.mensagem, buffer_resposta, sizeof(resposta.mensagem) - 1);
                        resposta.mensagem[sizeof(resposta.mensagem) - 1] = '\0';
                        
                        // caso a mensagem exceda os 2040 bytes (raro) adiciona-se '\n(...)' no final do texto
                        if (strlen(buffer_resposta) > 2040) {
                             strcat(resposta.mensagem, "\n(...)"); 
                        }
                    }

                    resposta.sucesso = 1;
                    pthread_mutex_unlock(&mutex_servicos);
                }
                else {
                    
                    sprintf(resposta.mensagem, "ERRO: Comando '%s' desconhecido.\n   Apenas existem os seguintes comandos:\n\t- agendar <hora> <local> <distancia>\n\t- cancelar <id>\n\t- consultar\n\t- entrar <destino>\n\t- sair\n\t- terminar\n\t- clear\n", cmd_tipo);
                    resposta.sucesso = 0;
                }
                printf("\n >>> ");
                fflush(stdout); 
            }

            // Envia a resposta de volta ao cliente pelo pipe privado
            write(fd_cli, &resposta, sizeof(MsgControladorCliente));
            
            // Fecha o pipe privado
            close(fd_cli);

        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {

    struct sigaction sa;
    sa.sa_handler = handler_desbloqueio; // Função vazia apenas para "tocar à campainha" (acordar a thread)
    sa.sa_flags = 0; // '0' desliga o restart automático. Ao meter '0', se o read for interrompido, dá erro (retorna -1) e não reinicia
    sigemptyset(&sa.sa_mask); // limpa a máscara (não bloqueia outros sinais enquanto se trata deste)
    sigaction(SIGUSR1, &sa, NULL); // aplica estas regras ao sinal SIGUSR1

    // ativar o sinal SIGINT (Ctrl+C) para chamar a função encerrar_sistema
    signal(SIGINT, encerrar_sistema);
    
    // 0660 = Permissões (Dono=RW, Grupo=RW, Outros=---)
    if (mkfifo(FIFO_CONTROLADOR, 0660) == -1) { // fifo publico criado aqui. Se o ficheiro já existe, ou outra informação anormal entra no IF
        if (errno == EEXIST) { // caso o fifo ja exista
            printf("[ERRO] Já existe uma instância do controlador a correr (PID desconhecido)!\n");
            exit(1);
        } else { // Outro erro sem ser que o ficheiro já existe
            perror("[CONTROLADOR-SV] Aviso: mkfifo");
            exit(1);
        }
    }

    // abre pipe público para Leitura e Escrita (O_RDWR) -> "truque" para que o servidor nunca receba EOF (fim de ficheiro) e continue a correr mesmo que nenhum cliente esteja conectado
    fd_sv = open(FIFO_CONTROLADOR, O_RDWR);
    if (fd_sv == -1) {
        perror("[CONTROLADOR-SV] ERRO FATAL ao abrir: " FIFO_CONTROLADOR);
        exit(1);
    }
    
    printf("[CONTROLADOR-SV] Servidor 'controlador' a arrancar (PID: %d)...\n", getpid());
    
    char *env = getenv("NVEICULOS");
    /* Formas de criar a variavel ambiente no cmd:
        1 - A variável fica na shell (apenas apaga-se quando se fecha a shell). Pode-se parar a aplicação e voltar a correr que sempre a vai ler.
            >>>   export NVEICULOS=5
            >>>   ./controlador
        2 - A variável fica apenas na aplicação (apaga-se quando a aplicação fechar). Ao parar a aplicação, a variavel ambiente desaparece.
            >>>   NVEICULOS=7 ./controlador */
    if (env != NULL) { // se a variavel ambiente 'NVEICULOS' existir, entra no if, se nao, interpreta que é o limite máximo definido: 10
        int v = atoi(env);
        if (v > 0 && v <= 10) nveiculos_max = v;
    }
    printf("[CONTROLADOR-SV] Frota máxima definida: %d veículos\n", nveiculos_max);
    
    // Lança a thread dos Clientes
    pthread_create(&tid_clientes, NULL, thread_client, NULL);
    
    usleep(100000); // espera 10% dum segundo para imprimir primeiro as msgs da thread do cliente e só dps escrever as do admin 
    
    // Lança a thread do Administrador
    pthread_create(&tid_admin, NULL, thread_admin_listener, NULL);
    
    // Lança a thread do Relógio
    pthread_create(&tid_relogio, NULL, thread_relogio, NULL);

    // Detach -> "Não vou esperar por ti"
    pthread_detach(tid_admin);
    pthread_detach(tid_relogio);
    
    // Join -> "Fico aqui preso até tu acabares". O Main só avança quando a thread_clientes terminar (no shutdown)
    pthread_join(tid_clientes, NULL); 

    encerrar_sistema(0);
    return 0;
}
