**Sistemas Operativos 2 — Trabalho Laboratorial**

**3ª Entrega (M3) — 31 de maio de 2026**



**===NÃO SE PRETENDE REALIZAR DEFESA ORAL!===**



**===REQUISITOS IMPLEMENTADOS===**



\-> Programa monitor.exe



\-> Janela principal Win32 — Criada com RegisterClassEx + CreateWindow, com menu associado via ficheiro de recursos .rc (lpszMenuName = MAKEINTRESOURCE(IDR\_MENU\_PRINCIPAL)). Implementa ciclo de mensagens com GetMessage / TranslateMessage / DispatchMessage.

Menu "Ficheiro" — Definido no ficheiro de recursos monitor.rc, com três entradas:



\-> Configuração — abre uma DialogBox (modal, definida no .rc) que permite definir o número máximo de alertas visíveis por página e os nomes dos recursos de comunicação/sincronização criados pelo central (SHM, evento, mutex).

\-> Acerca — mostra uma MessageBox com o nome e número dos alunos do grupo.

\-> Sair — termina a aplicação libertando todos os recursos (sem necessidade de avisar o central).





\-> Área de cliente — lista de alertas — Desenhada via WM\_PAINT com double-buffer (HDC em memória + BitBlt para evitar flicker). Apresenta um card por placar ativo, mostrando:



&#x09;Identificador do placar (Placar #N)

&#x09;Mensagem de alerta atual

&#x09;Duração em segundos





\-> Paginação — Quando o número de alertas ativos supera o máximo configurado, é possível navegar com Page Down (página seguinte) e Page Up (página anterior). A página atual e o total são indicados no rodapé.

\-> Leitura da memória partilhada — Abre a SHM criada pelo central com OpenFileMapping + MapViewOfFile. Abre o evento de notificação com OpenEvent e o mutex com OpenMutex, usando os nomes configurados.

\-> Thread de leitura da SHM — Aguarda em WaitForMultipleObjects com dois handles: o evento de notificação do central e um evento interno de shutdown. Quando o evento de notificação dispara:



\-> Tranca o mutex (WaitForSingleObject)

\-> Lê a estrutura SHM\_ALERTA e atualiza o estado interno

\-> Destrancar o mutex (ReleaseMutex)

\-> Força repintura da janela (InvalidateRect)

\-> Deteta desligar = TRUE na SHM e encerra graciosamente (fecha handles, limpa lista)





\-> Reconexão automática — Se o monitor for lançado antes do central, a thread tenta abrir a SHM a cada segundo até conseguir ligar, sem bloquear a interface gráfica.

\-> Encerramento — Ao fechar a janela (WM\_DESTROY / Ficheiro > Sair), sinaliza o evento interno de shutdown, aguarda o fim da thread e liberta todos os recursos: UnmapViewOfFile, CloseHandle do mapping, do evento e do mutex.





Programa central.exe (alterações M3)



\-> Memória partilhada (SHM) — Cria um bloco de memória partilhada com CreateFileMapping + MapViewOfFile com a estrutura SHM\_ALERTA (placar\[20] + desligar). Inicializada a zero no arranque (ZeroMemory).

\-> Sincronização com o monitor:



&#x09;Evento nomeado manual-reset (CreateEvent) — para notificar os processos monitor de alterações na SHM. Após cada SetEvent, é feito 	imediatamente ResetEvent para que apenas os monitores que estavam em espera sejam acordados.

&#x09;Mutex nomeado (CreateMutex) — para garantir acesso exclusivo à SHM entre o central e múltiplos monitores.





\-> Atualização da SHM nos eventos de alerta — A função AtualizaSHMeNotifica é chamada sempre que o estado dos alertas muda:



\-> Novo alerta confirmado (eco tipo 4): tranca mutex → atualiza entrada na SHM (id, duração, mensagem) → destrancar mutex → sinaliza evento

\-> Cancelar alerta confirmado (eco tipo 5): tranca mutex → limpa entrada do placar na SHM → destrancar mutex → sinaliza evento

\-> Fim de duração (tipo 3, recebido do placar): tranca mutex → limpa entrada do placar na SHM → destrancar mutex → sinaliza evento

\-> Placar perdeu ligação (pipe partido): idem ao cancelar





\-> Encerramento — Comando encerrar: atualiza SHM\_ALERTA.desligar = TRUE, sinaliza o evento (para informar todos os monitores), e depois liberta a SHM e todos os recursos associados (UnmapViewOfFile, CloseHandle do mapping, evento e mutex).





Programa placar.exe

Sem alterações relativamente à 2ª entrega.



Arquitetura de comunicação e sincronização

central  ──── Named Pipe (MSG\_ALERTA / MSG\_CMD) ────  placar(es)

&#x20;  │

&#x20;  │  SHM (SHM\_ALERTA)  +  Mutex   +  Evento 

&#x20;  │

monitor(es)



Central → Monitor: memória partilhada protegida por mutex; evento manual-reset para notificação imediata de alterações.

Central ↔ Placar: named pipes bidirecionais com Overlapped I/O (sem alterações ao M2).

