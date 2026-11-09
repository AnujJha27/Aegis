import { useEffect, useRef, useState } from "react";
import { api, type Agent, type AgentRun, type GitChange, type GitStatus, type Repository, type Task } from "./api";
import { Layout } from "../components/Layout";
import { useAgentWorkspace } from "./useAgentWorkspace";
import { usePtySession } from "./usePtySession";
import { useReviewData } from "./useReviewData";
import { useTasks } from "./useTasks";

export function App() {
  const taskState = useTasks();
  const { tasks, selectedTask, setSelectedTask } = taskState;
  const [drawer, setDrawer] = useState<"review" | "graphs" | "activity">("review");
  const [screen, setScreen] = useState<"session" | "review">("session");
  const workspace = useAgentWorkspace(selectedTask?.id, onEvent, reconnectRefresh);
  const review = useReviewData(selectedTask?.id, screen === "review" ? drawer : undefined);
  const [repository, setRepository] = useState<Repository>();
  const [agents, setAgents] = useState<Agent[]>([]);
  const [gitChanges, setGitChanges] = useState<GitChange[]>([]);
  const [gitStatus, setGitStatus] = useState<GitStatus>();
  const [selectedAgent, setSelectedAgent] = useState("shell");
  const [selectedModel, setSelectedModel] = useState("");
  const [prompt, setPrompt] = useState("");
  const [taskPrompt, setTaskPrompt] = useState("");
  const [openFileRequest, setOpenFileRequest] = useState({ path: "", token: 0 });
  const [busy, setBusy] = useState("Connecting to daemon…");
  const [sendingRunId, setSendingRunId] = useState("");
  const [daemonConnection, setDaemonConnection] = useState("connecting");
  const [error, setError] = useState("");
  const gitSequence = useRef(0);
  const repositoryVersion = useRef(0);
  const gitRefreshTimer = useRef<number | undefined>(undefined);
  const currentRun = workspace.currentRun;
  const selectedTaskIdRef = useRef(selectedTask?.id);
  const currentRunIdRef = useRef(currentRun?.id);
  selectedTaskIdRef.current = selectedTask?.id;
  currentRunIdRef.current = currentRun?.id;
  const runFinished = workspace.runFinished;
  const currentAgent = agents.find((agent) => agent.name === currentRun?.agent);
  const interactive = Boolean(currentAgent?.interactive);
  const pty = usePtySession(currentRun, interactive, runFinished);

  useEffect(() => { if (pty.error) setError(pty.error); }, [pty.error]);
  useEffect(() => { if (review.error) setError(review.error); }, [review.error]);
  useEffect(() => () => { ++gitSequence.current; window.clearTimeout(gitRefreshTimer.current); }, []);

  useEffect(() => {
    let active = true;
    let retryTimer = 0;
    let retries = 0;
    const load = async () => {
      try {
        const [, repo, loadedAgents, git] = await Promise.all([taskState.refresh(), api.repository(), api.agents(), api.gitStatus()]);
        if (!active) return;
        setRepository(repo);
        setAgents(loadedAgents);
        setSelectedAgent(loadedAgents.find((agent) => agent.available)?.name ?? "shell");
        setGitChanges(git.files);
        setGitStatus(git);
        setError("");
        setBusy("Ready");
        setDaemonConnection("connected");
      } catch (reason) {
        if (!active) return;
        setError(reason instanceof Error ? reason.message : "Could not connect to daemon");
        setBusy("Daemon unavailable");
        setDaemonConnection("unavailable");
        const delay = Math.min(500 * 2 ** retries++, 8000);
        retryTimer = window.setTimeout(() => {
          setBusy("Reconnecting…");
          setDaemonConnection("reconnecting");
          void load();
        }, delay);
      }
    };
    void load();
    return () => { active = false; window.clearTimeout(retryTimer); };
  }, [taskState.refresh]);

  function changeAgent(agent: string) {
    setSelectedAgent(agent);
    setSelectedModel("");
  }

  function onEvent(event: import("./api").AgentEvent) {
    if (event.type === "terminal.output") window.dispatchEvent(new CustomEvent("aegis:terminal-output", { detail: event }));
    if (event.runId === currentRunIdRef.current && event.type === "turn.started") setBusy("Agent working…");
    else if (event.runId === currentRunIdRef.current && event.type === "turn.completed") setBusy("Ready for next prompt");
    else if (event.runId === currentRunIdRef.current && (event.type === "turn.interrupted" || event.type === "run.interrupted")) setBusy("Agent interrupted");
    else if (event.runId === currentRunIdRef.current && event.type === "run.completed") setBusy("Run complete");
    else if (event.runId === currentRunIdRef.current && event.type === "run.failed") setBusy("Agent failed");
    else if (event.runId === currentRunIdRef.current && event.type === "run.terminated") setBusy("Agent stopped");
    if (event.type === "file.changed" || event.type === "repository.changed") {
      if (event.type === "repository.changed") {
        try {
          const update = JSON.parse(event.content) as { version: number; index_version: number };
          if (update.version <= repositoryVersion.current) return;
          repositoryVersion.current = update.version;
          window.dispatchEvent(new CustomEvent("aegis:repository", { detail: update }));
        } catch { return; }
      }
      window.clearTimeout(gitRefreshTimer.current);
      gitRefreshTimer.current = window.setTimeout(() => void refreshGit().catch((reason: Error) => setError(reason.message)), 100);
    }
  }

  async function refreshGit() {
    const sequence = ++gitSequence.current;
    const version = repositoryVersion.current;
    const git = await api.gitStatus();
    if (sequence !== gitSequence.current || version !== repositoryVersion.current) return;
    if (JSON.stringify(git.repository) !== JSON.stringify(repository)) setRepository(git.repository);
    if (JSON.stringify(git.files) !== JSON.stringify(gitChanges)) setGitChanges(git.files);
    if (JSON.stringify(git) !== JSON.stringify(gitStatus)) setGitStatus(git);
    await review.refresh();
  }

  async function reconnectRefresh() {
    repositoryVersion.current = 0;
    window.dispatchEvent(new Event("aegis:resync"));
    try { await refreshGit(); setDaemonConnection("connected"); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Could not refresh repository state"); setDaemonConnection("unavailable"); }
  }

  async function createTask() {
    if (!taskPrompt.trim()) return;
    try {
      await taskState.create(taskPrompt.trim());
      setTaskPrompt("");
    } catch (reason) { setError(reason instanceof Error ? reason.message : "Could not create task"); }
  }

  async function launch() {
    const taskId = selectedTask?.id;
    const agent = selectedAgent;
    if (!taskId) return;
    setBusy(selectedModel ? `Starting ${agent} (${selectedModel})…` : `Starting ${agent}…`);
    try {
      const run = await api.launch(taskId, agent, selectedModel || undefined);
      if (selectedTaskIdRef.current !== taskId) return;
      workspace.setRuns((current) => [...current, run]);
      workspace.setSelectedRunId(run.id);
      setBusy(`${agent} active`);
    } catch (reason) {
      if (selectedTaskIdRef.current !== taskId) return;
      setError(reason instanceof Error ? reason.message : "Could not start agent");
      setBusy("Start failed");
    }
  }

  async function send() {
    if (!currentRun || !prompt.trim()) return;
    const taskId = currentRun.taskId;
    const runId = currentRun.id;
    const message = prompt.trim();
    setSendingRunId(runId);
    setBusy("Agent working…");
    try {
      await api.send(runId, message);
      if (selectedTaskIdRef.current !== taskId || currentRunIdRef.current !== runId) return;
      setPrompt("");
    } catch (reason) {
      if (selectedTaskIdRef.current !== taskId || currentRunIdRef.current !== runId) return;
      setError(reason instanceof Error ? reason.message : "Could not send prompt");
      setBusy("Send failed");
    } finally {
      setSendingRunId((current) => current === runId ? "" : current);
    }
  }

  async function deleteRun(runId: string) {
    const run = workspace.runs.find((item) => item.id === runId);
    if (!run || !["completed", "failed", "interrupted", "terminated"].includes(run.status)) return;
    const taskId = run.taskId;
    const shortId = runId.slice(0, 8);
    try {
      await api.deleteRun(runId);
      if (selectedTaskIdRef.current !== taskId) return;
      const remaining = workspace.runs.filter((item) => item.id !== runId);
      workspace.setRuns((current) => current.filter((item) => item.id !== runId));
      workspace.setEvents((current) => current.filter((event) => event.runId !== runId));
      if (workspace.selectedRunId === runId) workspace.setSelectedRunId(remaining.at(-1)?.id ?? "");
      await review.refresh();
    } catch (reason) {
      if (selectedTaskIdRef.current !== taskId) return;
      setError(reason instanceof Error ? `${reason.message} (run ${shortId})` : `Could not delete run ${shortId}`);
    }
  }

  async function interruptRun(runId: string) {
    const run = workspace.runs.find((item) => item.id === runId);
    const adapter = agents.find((item) => item.name === run?.agent);
    if (!run || !adapter?.interruptible || !["starting", "running"].includes(run.status) ||
        !(run.status === "starting" || workspace.turnBusy || adapter.interactive)) return;
    const taskId = run.taskId;
    const shortId = runId.slice(0, 8);
    setBusy("Interrupting turn…");
    try { await api.interrupt(runId); }
    catch (reason) {
      if (selectedTaskIdRef.current !== taskId) return;
      setError(reason instanceof Error ? `${reason.message} (run ${shortId})` : `Could not interrupt agent ${shortId}`);
      setBusy("Interrupt failed");
    }
  }

  async function terminateRun(runId: string) {
    const run = workspace.runs.find((item) => item.id === runId);
    if (!run || !["starting", "running"].includes(run.status)) return;
    const taskId = run.taskId;
    const shortId = runId.slice(0, 8);
    setBusy("Stopping agent…");
    try {
      await api.terminate(runId);
      if (selectedTaskIdRef.current !== taskId) return;
      workspace.setRuns((current) => current.map((item) => item.id === runId ? { ...item, status: "terminated", finishedAt: Date.now() } : item));
      setBusy("Agent stopped");
    } catch (reason) {
      if (selectedTaskIdRef.current !== taskId) return;
      setError(reason instanceof Error ? `${reason.message} (run ${shortId})` : `Could not stop agent ${shortId}`);
      setBusy("Stop failed");
    }
  }

  async function verify(command?: string[]) {
    const taskId = selectedTask?.id;
    if (!taskId) return;
    setBusy("Verifying…");
    try {
      await review.verify(currentRun?.id, command);
      if (selectedTaskIdRef.current !== taskId) return;
      setBusy("Ready");
    } catch (reason) {
      if (selectedTaskIdRef.current !== taskId) return;
      setError(reason instanceof Error ? `${reason.message} (task ${taskId.slice(0, 8)})` : `Verification failed for task ${taskId.slice(0, 8)}`);
      setBusy("Verification failed");
    }
  }

  function onContinued(run: AgentRun, sourceRunId: string) {
    if (run.taskId !== selectedTaskIdRef.current) return;
    workspace.setRuns((current) => current.some((item) => item.id === run.id) ? current : [...current, run]);
    if (currentRunIdRef.current === sourceRunId) workspace.setSelectedRunId(run.id);
  }

  async function onImportedTask(task: Task) {
    setSelectedTask(task);
    try { await taskState.refresh(); } catch (reason) { setError(reason instanceof Error ? reason.message : "Could not refresh tasks"); }
  }

  function openReviewFile(path: string) {
    setOpenFileRequest((current) => ({ path, token: current.token + 1 }));
    setDrawer("review");
    setScreen("review");
  }

  return <Layout
    repository={repository} tasks={tasks} selectedTask={selectedTask} onSelectTask={setSelectedTask}
    taskPrompt={taskPrompt} onTaskPrompt={setTaskPrompt} onCreateTask={createTask}
    agents={agents} selectedAgent={selectedAgent} onAgentChange={changeAgent} onLaunch={launch} selectedModel={selectedModel} onModelChange={setSelectedModel} canLaunch={Boolean(selectedTask && agents.some((agent) => agent.name === selectedAgent && agent.available))}
    run={currentRun} runs={workspace.runs} onSelectRun={workspace.setSelectedRunId} onDeleteRun={deleteRun} onInterruptRun={interruptRun} onTerminateRun={terminateRun} actionsBusy={busy === "Interrupting turn…" || busy === "Stopping agent…"} runFinished={runFinished} turnBusy={workspace.turnBusy} turnCompleted={workspace.turnCompleted} turnInterrupted={workspace.turnInterrupted} resumable={Boolean(currentAgent?.resumable)}
    onPtyInput={pty.send} onPtyResize={pty.resize} ptyConnection={pty.connection} events={workspace.currentRunEvents} activityEvents={workspace.currentEvents}
    prompt={prompt} onPrompt={setPrompt} onSend={send} busy={busy} sending={sendingRunId === currentRun?.id}
    screen={screen} onScreen={setScreen} drawer={drawer} onDrawer={setDrawer} connection={selectedTask ? workspace.connection : daemonConnection}
    gitChanges={gitChanges} gitStatus={gitStatus} onGitChanged={refreshGit}
    verification={review.verification} handoff={review.handoff} graph={review.graph} provenance={review.provenance}
    onVerify={verify} error={error} onDismissError={() => setError("")} openFilePath={openFileRequest.path} openFileToken={openFileRequest.token} onOpenFile={openReviewFile}
    onContinued={onContinued} onTaskUpdated={taskState.update} onImported={onImportedTask} />;
}
