import { useEffect, useState } from "react";
import { api, type Agent, type GitChange, type GitStatus, type Repository, type Task } from "./api";
import { Layout } from "../components/Layout";
import { useAgentWorkspace } from "./useAgentWorkspace";
import { usePtySession } from "./usePtySession";
import { useReviewData } from "./useReviewData";
import { useTasks } from "./useTasks";

export function App() {
  const taskState = useTasks();
  const { tasks, selectedTask, setSelectedTask } = taskState;
  const workspace = useAgentWorkspace(selectedTask?.id, onEvent, reconnectRefresh);
  const review = useReviewData(selectedTask?.id);
  const [repository, setRepository] = useState<Repository>();
  const [agents, setAgents] = useState<Agent[]>([]);
  const [gitChanges, setGitChanges] = useState<GitChange[]>([]);
  const [gitStatus, setGitStatus] = useState<GitStatus>();
  const [selectedAgent, setSelectedAgent] = useState("shell");
  const [prompt, setPrompt] = useState("");
  const [taskPrompt, setTaskPrompt] = useState("");
  const [drawer, setDrawer] = useState<"review" | "graphs" | "activity">("review");
  const [openFileRequest, setOpenFileRequest] = useState({ path: "", token: 0 });
  const [screen, setScreen] = useState<"session" | "review">("session");
  const [busy, setBusy] = useState("Connecting to daemon…");
  const [daemonConnection, setDaemonConnection] = useState("connecting");
  const [error, setError] = useState("");
  const currentRun = workspace.currentRun;
  const runFinished = workspace.runFinished;
  const interactive = Boolean(agents.find((agent) => agent.name === currentRun?.agent)?.interactive);
  const pty = usePtySession(currentRun, interactive, runFinished);

  useEffect(() => { if (review.error) setError(review.error); }, [review.error]);

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

  function onEvent(event: import("./api").AgentEvent) {
    if (["run.completed", "run.failed", "run.interrupted", "run.terminated", "turn.completed", "turn.interrupted"].includes(event.type))
      setBusy(event.type === "run.failed" ? "Agent failed" : "Ready");
    if (event.type === "file.changed") void refreshGit().catch((reason: Error) => setError(reason.message));
  }

  async function refreshGit() {
    const git = await api.gitStatus();
    setRepository(git.repository);
    setGitChanges(git.files);
    setGitStatus(git);
    await review.refresh();
  }

  async function reconnectRefresh() {
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
    if (!selectedTask) return;
    setBusy(`Starting ${selectedAgent}…`);
    try {
      const run = await api.launch(selectedTask.id, selectedAgent);
      workspace.setRuns((current) => [...current, run]);
      workspace.setSelectedRunId(run.id);
      setBusy(`${selectedAgent} active`);
    } catch (reason) { setError(reason instanceof Error ? reason.message : "Could not start agent"); setBusy("Start failed"); }
  }

  async function send() {
    if (!currentRun || !prompt.trim()) return;
    const message = prompt.trim();
    try {
      await api.send(currentRun.id, message);
      setPrompt("");
      setBusy("Agent working…");
    } catch (reason) { setError(reason instanceof Error ? reason.message : "Could not send prompt"); }
  }

  async function deleteRun(runId: string) {
    const run = workspace.runs.find((item) => item.id === runId);
    if (!run || !["completed", "failed", "interrupted", "terminated"].includes(run.status)) return;
    try {
      await api.deleteRun(runId);
      const remaining = workspace.runs.filter((item) => item.id !== runId);
      workspace.setRuns(remaining);
      workspace.setEvents((current) => current.filter((event) => event.runId !== runId));
      if (workspace.selectedRunId === runId) workspace.setSelectedRunId(remaining.at(-1)?.id ?? "");
      await review.refresh();
    } catch (reason) { setError(reason instanceof Error ? reason.message : "Could not delete run"); }
  }

  async function verify() {
    if (!selectedTask) return;
    setBusy("Verifying…");
    try { await review.verify(currentRun?.id); setBusy("Ready"); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Verification failed"); setBusy("Verification failed"); }
  }

  function openReviewFile(path: string) {
    setOpenFileRequest((current) => ({ path, token: current.token + 1 }));
    setDrawer("review");
    setScreen("review");
  }

  return <Layout
    repository={repository} tasks={tasks} selectedTask={selectedTask} onSelectTask={setSelectedTask}
    taskPrompt={taskPrompt} onTaskPrompt={setTaskPrompt} onCreateTask={createTask}
    agents={agents} selectedAgent={selectedAgent} onAgentChange={setSelectedAgent} onLaunch={launch}
    run={currentRun} runs={workspace.runs} onSelectRun={workspace.setSelectedRunId} onDeleteRun={deleteRun} runFinished={runFinished}
    onPtyInput={pty.send} onPtyResize={pty.resize} ptyConnection={pty.connection} events={workspace.currentRunEvents} activityEvents={workspace.currentEvents}
    prompt={prompt} onPrompt={setPrompt} onSend={send} busy={busy}
    screen={screen} onScreen={setScreen} drawer={drawer} onDrawer={setDrawer} connection={selectedTask ? workspace.connection : daemonConnection}
    gitChanges={gitChanges} gitStatus={gitStatus} onGitChanged={refreshGit}
    verification={review.verification} handoff={review.handoff} graph={review.graph} provenance={review.provenance}
    onVerify={verify} error={error} onDismissError={() => setError("")} openFilePath={openFileRequest.path} openFileToken={openFileRequest.token} onOpenFile={openReviewFile} />;
}
