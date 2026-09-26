import { useEffect, useMemo, useRef, useState } from "react";
import { api, type Agent, type AgentEvent, type AgentRun, type GitChange, type GitStatus, type HandoffContext, type ProvenanceRecord, type Repository, type Task, type TaskGraph, type VerificationRun } from "./api";
import { connectEvents, connectPty } from "./events";
import { Layout } from "../components/Layout";

export function App() {
  const [repository, setRepository] = useState<Repository>();
  const [tasks, setTasks] = useState<Task[]>([]);
  const [agents, setAgents] = useState<Agent[]>([]);
  const [selectedTask, setSelectedTask] = useState<Task>();
  const [runs, setRuns] = useState<AgentRun[]>([]);
  const [selectedRunId, setSelectedRunId] = useState("");
  const [events, setEvents] = useState<AgentEvent[]>([]);
  const [diff, setDiff] = useState("");
  const [gitChanges, setGitChanges] = useState<GitChange[]>([]);
  const [gitStatus, setGitStatus] = useState<GitStatus>();
  const [verification, setVerification] = useState<VerificationRun>();
  const [handoff, setHandoff] = useState<HandoffContext>();
  const [graph, setGraph] = useState<TaskGraph>();
  const [provenance, setProvenance] = useState<ProvenanceRecord[]>([]);
  const [selectedAgent, setSelectedAgent] = useState("shell");
  const [prompt, setPrompt] = useState("");
  const [taskPrompt, setTaskPrompt] = useState("");
  const [drawer, setDrawer] = useState<"review" | "graphs" | "activity" | "handoff">();
  const [screen, setScreen] = useState<"session" | "review">("session");
  const [busy, setBusy] = useState("Connecting to daemon…");
  const [error, setError] = useState("");
  const ptySocket = useRef<WebSocket | null>(null);
  const ptyPending = useRef<string[]>([]);

  const currentRun = runs.find((run) => run.id === selectedRunId) ?? runs.at(-1);
  const currentEvents = useMemo(() => events.filter((event) => !selectedTask || event.taskId === selectedTask.id), [events, selectedTask]);
  const runFinished = currentRun ? events.some((event) => event.runId === currentRun.id && (event.type === "agent.finished" || event.type === "agent.failed")) : false;

  useEffect(() => {
    Promise.all([api.repository(), api.tasks(), api.agents(), api.changes(), api.gitStatus()])
      .then(([repo, loadedTasks, loadedAgents, changes, git]) => {
        setRepository(repo);
        setTasks(loadedTasks);
        setSelectedTask(loadedTasks[0]);
        setAgents(loadedAgents);
        setDiff(changes.diff);
        setGitChanges(git.files);
        setGitStatus(git);
        setBusy("Ready");
        if (loadedAgents.find((agent) => agent.available)?.name) setSelectedAgent(loadedAgents.find((agent) => agent.available)!.name);
      })
      .catch((reason: Error) => { setError(reason.message); setBusy("Daemon unavailable"); });
    return connectEvents((event) => {
      setEvents((current) => [...current.slice(-499), event]);
      if (event.type === "agent.finished" || event.type === "agent.failed") setBusy(event.type === "agent.failed" ? "Agent failed" : "Ready");
      if (event.type === "file.changed") void refreshGit(event.taskId).catch((reason: Error) => setError(reason.message));
    });
  }, []);

  useEffect(() => {
    if (!selectedTask) return;
    setRuns([]);
    setSelectedRunId("");
    Promise.all([api.events(selectedTask.id), api.runs(selectedTask.id), api.changes(), api.handoff(selectedTask.id), api.graph(selectedTask.id), api.provenance(selectedTask.id)]).then(([loadedEvents, loadedRuns, changes, loadedHandoff, loadedGraph, loadedProvenance]) => { setEvents(loadedEvents); setRuns(loadedRuns); setSelectedRunId(loadedRuns.at(-1)?.id ?? ""); setDiff(changes.diff); setHandoff(loadedHandoff); setGraph(loadedGraph); setProvenance(loadedProvenance.records); });
  }, [selectedTask]);

  useEffect(() => {
    if (!currentRun || !agents.find((agent) => agent.name === currentRun.agent)?.interactive || runFinished) {
      ptySocket.current?.close();
      ptySocket.current = null;
      return;
    }
    ptyPending.current = [];
    const socket = connectPty(currentRun.id);
    ptySocket.current = socket;
    socket.onopen = () => {
      for (const input of ptyPending.current) socket.send(input);
      ptyPending.current = [];
    };
    return () => { socket.close(); if (ptySocket.current === socket) ptySocket.current = null; };
  }, [currentRun?.id, agents, runFinished]);

  async function refreshGit(taskId = selectedTask?.id) {
    const [git, changes] = await Promise.all([api.gitStatus(), api.changes()]);
    setRepository(git.repository);
    setGitChanges(git.files);
    setGitStatus(git);
    setDiff(changes.diff);
    if (taskId) {
      const [loadedHandoff, loadedGraph, loadedProvenance] = await Promise.all([api.handoff(taskId), api.graph(taskId), api.provenance(taskId)]);
      setHandoff(loadedHandoff);
      setGraph(loadedGraph);
      setProvenance(loadedProvenance.records);
    }
  }

  async function createTask() {
    if (!taskPrompt.trim()) return;
    try {
      const task = await api.createTask(taskPrompt.trim());
      setTasks((current) => [task, ...current]);
      setSelectedTask(task);
      setTaskPrompt("");
    } catch (reason) { setError(reason instanceof Error ? reason.message : "Could not create task"); }
  }

  async function launch() {
    if (!selectedTask) return;
    setBusy(`Starting ${selectedAgent}…`);
    try {
      const run = await api.launch(selectedTask.id, selectedAgent);
      setRuns((current) => [...current, run]);
      setSelectedRunId(run.id);
      setBusy(`${selectedAgent} active`);
    } catch (reason) { setError(reason instanceof Error ? reason.message : "Could not start agent"); setBusy("Start failed"); }
  }

  async function send() {
    if (!currentRun || !prompt.trim()) return;
    const message = prompt.trim();
    setPrompt("");
    try { await api.send(currentRun.id, message); setBusy("Agent working…"); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Could not send prompt"); }
  }

  async function verify() {
    setBusy("Verifying…");
    try { setVerification(await api.verify(["ctest", "--test-dir", "build"])); setBusy("Ready"); }
    catch (reason) { setError(reason instanceof Error ? reason.message : "Verification failed"); setBusy("Verification failed"); }
  }

  return <Layout
    repository={repository} tasks={tasks} selectedTask={selectedTask} onSelectTask={setSelectedTask}
    taskPrompt={taskPrompt} onTaskPrompt={setTaskPrompt} onCreateTask={createTask}
    agents={agents} selectedAgent={selectedAgent} onAgentChange={setSelectedAgent} onLaunch={launch}
    run={currentRun} runs={runs} onSelectRun={setSelectedRunId} runFinished={runFinished} onPtyInput={(input) => { if (ptySocket.current?.readyState === WebSocket.OPEN) ptySocket.current.send(input); else if (ptySocket.current?.readyState === WebSocket.CONNECTING) ptyPending.current.push(input); }} events={currentEvents} prompt={prompt} onPrompt={setPrompt} onSend={send} busy={busy}
    screen={screen} onScreen={setScreen} drawer={drawer ?? "review"} onDrawer={setDrawer} gitChanges={gitChanges} gitStatus={gitStatus} onGitChanged={refreshGit} diff={diff} verification={verification} handoff={handoff} graph={graph} provenance={provenance} onVerify={verify} error={error} />;
}
